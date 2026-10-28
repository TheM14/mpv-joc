// mpv C plugin: E-AC-3 JOC (Dolby Atmos) playback.
//
// mpv decodes the file's video and its E-AC-3 bed normally; this plugin adds a
// second audio track that carries the object-audio rendering.  The rendering
// itself is the same kernel, the same engine and the same settings as the
// upstream frontends, so the delivered samples match them bit for bit.
//
// The track is served over a loopback HTTP range endpoint backed by the
// renderer: mpv reads it as an ordinary seekable WAV, which is what lets its
// demuxer, cache, A/V sync, WASAPI output and seeking all keep working.

#include <winsock2.h>
#include <windows.h>

// Every mpv_* call goes through the host's exported symbol table: a cplugin
// must not link against libmpv.
#ifndef MPV_CPLUGIN_DYNAMIC_SYM
#define MPV_CPLUGIN_DYNAMIC_SYM
#endif
#include <mpv/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "container_scan.h"
#include "joc_decode.h"
#include "log.h"
#include "win_path.h"
#include "plugin/audio_source.h"
#include "plugin/config.h"
#include "plugin/http_server.h"
#include "plugin/wav.h"

namespace {

constexpr const char* kResource = "/joc.wav";

std::string plugin_directory() { return joc_decode::component_directory(); }

bool file_exists(const std::string& path) {
    std::wstring wide;
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (needed <= 1) return false;
    wide.resize(static_cast<std::size_t>(needed));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), needed);
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// The default HRTF lives next to the plugin, or one level up when the plugin
// sits in <mpv>\scripts and the data in <mpv> itself.
std::string default_hrtf_file(const joc_mpv::Config& config) {
    const char* name = (config.hrtf_source == 0) ? "binaural.sofa"
                                                 : "binaural.personalized_headphone";
    const std::string directory = plugin_directory();
    const std::string candidates[] = {directory + "\\HRTF\\" + name,
                                      directory + "\\..\\HRTF\\" + name};
    for (const std::string& candidate : candidates) {
        if (file_exists(candidate)) return candidate;
    }
    return {};
}

// Sent as a node array rather than a command string: an error message carries
// backslashes and non-ASCII text, which the command parser would mangle.
void osd(mpv_handle* mpv, const std::string& text) {
    mpv_node values[3];
    values[0].format = MPV_FORMAT_STRING;
    values[0].u.string = const_cast<char*>("show-text");
    values[1].format = MPV_FORMAT_STRING;
    values[1].u.string = const_cast<char*>(text.c_str());
    values[2].format = MPV_FORMAT_STRING;
    values[2].u.string = const_cast<char*>("3000");
    mpv_node_list list{};
    list.num = 3;
    list.values = values;
    mpv_node command{};
    command.format = MPV_FORMAT_NODE_ARRAY;
    command.u.list = &list;
    mpv_command_node(mpv, &command, nullptr);
}

// Byte offset of the first syncframe.  A bare stream may start with an ID3v2
// tag, which the renderer refuses to parse as audio.
std::uint64_t leading_tag_bytes(const std::string& path) {
    FILE* file = _wfopen(joc_path::to_wide_extended(path).c_str(), L"rb");
    if (file == nullptr) return 0;
    unsigned char header[10] = {};
    const std::size_t got = std::fread(header, 1, sizeof(header), file);
    std::fclose(file);
    if (got != sizeof(header) || std::memcmp(header, "ID3", 3) != 0) return 0;
    auto synchsafe = [](unsigned char a, unsigned char b, unsigned char c, unsigned char d) {
        return (static_cast<std::uint64_t>(a & 0x7F) << 21) |
               (static_cast<std::uint64_t>(b & 0x7F) << 14) |
               (static_cast<std::uint64_t>(c & 0x7F) << 7) |
               static_cast<std::uint64_t>(d & 0x7F);
    };
    const std::uint64_t size = synchsafe(header[6], header[7], header[8], header[9]);
    // The footer flag (0x10) adds another 10 bytes after the tag body.
    const std::uint64_t footer = (header[5] & 0x10) != 0 ? 10u : 0u;
    return 10u + size + footer;
}

std::string format_channels(unsigned channels, const std::string& layout, int output) {
    if (output == 0) return "binaural 2ch";
    return layout + " " + std::to_string(channels) + "ch";
}

// The WAV the plugin serves: header plus the rendered data chunk.
struct Stream {
    std::shared_ptr<joc_mpv::Source> source;
    std::string header;
    std::uint64_t total_bytes = 0;
    std::size_t write_bytes = 65536;
    bool verbose = false;
    // Only the newest request may read from the renderer: a request that mpv
    // replaces (a seek closes the old stream and opens a new one) has to stop
    // instead of pulling the renderer back to its own position.
    std::atomic<unsigned> generation{0};
};

// Streams the requested part of the file until the client closes the
// connection or the audio ends.  mpv seeks by reopening the URL with a new
// Range, which becomes a renderer seek.
void serve_request(const std::shared_ptr<Stream>& stream, const joc_mpv::HttpRequest& request,
                   joc_mpv::HttpResponse* response) {
    if (request.method != "GET") {
        response->status = 400;
        return;
    }
    if (request.target.compare(0, std::strlen(kResource), kResource) != 0) {
        response->status = 404;
        return;
    }
    const std::uint64_t header_size = stream->header.size();
    const std::uint64_t total = stream->total_bytes;
    const std::uint64_t start = request.has_range ? request.range_start : 0;
    if (start >= total) {
        response->status = 416;
        response->headers = "Content-Range: bytes */" + std::to_string(total) + "\r\n";
        return;
    }
    response->status = request.has_range ? 206 : 200;
    response->headers = "Content-Type: audio/wav\r\n";
    response->headers += "Accept-Ranges: bytes\r\n";
    if (request.has_range) {
        response->headers += "Content-Range: bytes " + std::to_string(start) + "-" +
                             std::to_string(total - 1) + "/" + std::to_string(total) + "\r\n";
    }
    if (stream->verbose) {
        joc_log::line("http: %s %s from %llu of %llu", request.method.c_str(),
                      request.target.c_str(), static_cast<unsigned long long>(start),
                      static_cast<unsigned long long>(total));
    }
    const unsigned generation = stream->generation.fetch_add(1) + 1;
    const std::uint64_t last =
        (request.has_range && request.has_range_end)
            ? std::min<std::uint64_t>(request.range_end, total - 1)
            : total - 1;
    response->stream = [stream, start, last, header_size, total, generation](
                           joc_mpv::ResponseWriter& writer) {
        const unsigned frame_bytes = stream->source->bytes_per_frame();
        std::vector<std::uint8_t> buffer(stream->write_bytes);
        std::uint64_t offset = start;
        // The WAV header is one chunk of its own: from the first data byte on,
        // every offset is a whole number of frames, so the renderer never has to
        // reposition just because a write did not end on a frame boundary.
        if (offset < header_size) {
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(header_size - offset, last + 1 - offset));
            if (!writer.write(stream->header.data() + offset, take)) return;
            offset += take;
        }
        while (offset <= last) {
            if (stream->generation.load() != generation) break;
            std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), last + 1 - offset));
            want -= want % frame_bytes;
            if (want == 0) break;
            std::string error;
            const std::size_t got =
                stream->source->read(offset - header_size, buffer.data(), want, &error);
            if (got == 0) {
                if (stream->verbose) {
                    joc_log::line("http: stream ended at %llu: %s",
                                  static_cast<unsigned long long>(offset), error.c_str());
                }
                break;
            }
            if (!writer.write(buffer.data(), got)) break;
            offset += got;
        }
    };
}

struct Plugin {
    mpv_handle* mpv = nullptr;
    joc_mpv::Config config;
    std::unique_ptr<joc_mpv::HttpServer> server;
    std::shared_ptr<Stream> stream;
    std::string current_path;   // the file a stream was prepared for
    std::string url;
    // The last file a probe answered for, so a rejected input is not probed a
    // second time at file-loaded.
    std::string probed_path;
    bool probed_joc = false;
    std::string config_dir;
    // The file's own audio is muted (when the configuration asks for it) from
    // the first sample until the rendered track takes over, so the opening of a
    // JOC file is never the plain bed.  The player's own mute state is restored.
    bool muted_by_us = false;
    bool saved_mute = false;

    void stop_stream() {
        if (stream) stream->generation.fetch_add(1);
        if (server) server->stop();
        if (stream && stream->source) stream->source->close();
        stream.reset();
        server.reset();
        current_path.clear();
        url.clear();
    }

    bool muted_state() {
        int flag = 0;
        if (mpv_get_property(mpv, "mute", MPV_FORMAT_FLAG, &flag) < 0) return false;
        return flag != 0;
    }

    void set_muted(bool muted) {
        int flag = muted ? 1 : 0;
        mpv_set_property(mpv, "mute", MPV_FORMAT_FLAG, &flag);
    }

    void suppress_bed() {
        if (!config.mute_until_ready || muted_by_us) return;
        saved_mute = muted_state();
        if (saved_mute) return;  // the player is already silent; nothing to restore
        set_muted(true);
        muted_by_us = true;
    }

    void restore_bed() {
        if (!muted_by_us) return;
        set_muted(saved_mute);
        muted_by_us = false;
    }

    // Only inputs that can carry E-AC-3 JOC are worth silencing before the
    // probe answers; anything else would lose its first moments to a mute.
    static bool candidate(const std::string& path) {
        const std::size_t dot = path.find_last_of('.');
        if (dot == std::string::npos) return false;
        std::string extension = path.substr(dot + 1);
        for (char& ch : extension) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        static const char* const kExtensions[] = {"eac3", "ec3", "ac3", "mp4", "m4a", "m4b",
                                                  "mov",  "mkv", "mka", "webm"};
        for (const char* known : kExtensions) {
            if (extension == known) return true;
        }
        return false;
    }

    bool probe(const std::string& path, joc_decode::Settings* settings, double* duration) {
        const std::uint64_t tag_bytes = leading_tag_bytes(path);
        const joc_decode::FileProbe bare = joc_decode::probe_file(path, 0, tag_bytes);
        if (bare.readable && bare.joc) {
            settings->input_kind = joc_decode::InputKind::kBare;
            settings->stream_start_bytes = tag_bytes;
            *duration = bare.duration_seconds;
            joc_log::line("probe: %s is a JOC stream (%llu frames, %.3f s)", path.c_str(),
                          static_cast<unsigned long long>(bare.frames), *duration);
            return true;
        }
        const joc_container::Result container = joc_container::scan(path);
        if (container.kind == joc_container::Kind::kNone || !container.eac3) {
            joc_log::line("probe: %s is not an E-AC-3 JOC input (%s)", path.c_str(),
                          container.detail.c_str());
            return false;
        }
        bool verdict = false;
        std::string detail;
        if (!joc_decode::probe_container_joc(config.ffmpeg, path, container.audio_index, &verdict,
                                             &detail) ||
            !verdict) {
            joc_log::line("probe: container %s track %u is not JOC (%s)", path.c_str(),
                          container.audio_index, detail.c_str());
            return false;
        }
        settings->input_kind = joc_decode::InputKind::kContainer;
        settings->audio_index = container.audio_index;
        *duration = container.duration_seconds;
        joc_log::line("probe: %s carries JOC in audio track %u (%.3f s)", path.c_str(),
                      container.audio_index, *duration);
        return true;
    }

    // Starts the renderer and its endpoint for a file, without touching the
    // player yet: at start-file the entry is not loaded, so audio-add could only
    // be applied to the previous one.  Returns true when a stream is ready.
    bool prepare(const std::string& path) {
        stop_stream();
        probed_path = path;
        probed_joc = false;
        if (!config.enabled) return false;

        joc_decode::Settings settings;
        settings.output = (config.output == 0) ? joc_decode::Output::kBinaural
                                               : joc_decode::Output::kSpeaker;
        settings.speaker_layout = config.layout;
        settings.hrtf_source = (config.hrtf_source == 0) ? joc_decode::HrtfSource::kSofa
                                                         : joc_decode::HrtfSource::kRosella;
        settings.hrtf_file = config.hrtf_file;
        if (settings.hrtf_file.empty() && settings.output == joc_decode::Output::kBinaural) {
            settings.hrtf_file = default_hrtf_file(config);
        }
        settings.binaural_mode = static_cast<std::uint32_t>(config.binaural_mode);
        settings.gain_db = config.gain_db;
        settings.tail_seconds = config.tail_seconds;
        settings.force_int24 = config.force_int24;
        settings.object_delay_samples = config.object_delay_samples;
        settings.native_threads = config.native_threads;
        settings.ffmpeg_path = config.ffmpeg;
        settings.core_path = config.core_dll;
        // The compiled SOFA field is the dominant part of the startup cost;
        // keeping it on disk makes every later file (and every seek restart-free
        // start) skip that work.
        if (config.verbose) {
            joc_log::line("config: hrtf-cache=%s dir=%s", config.hrtf_cache.c_str(),
                          config.hrtf_cache_dir.c_str());
        }
        if (config.hrtf_cache == "none") {
            settings.hrtf_cache_policy = 0;
        } else if (config.hrtf_cache == "memory") {
            settings.hrtf_cache_policy = 1;
        } else {
            // disk and auto: the config directory is the natural place for a
            // cache; without one the renderer compiles in memory as before.
            std::string directory = config.hrtf_cache_dir;
            if (directory.empty() && !config_dir.empty()) {
                directory = config_dir + "\\joc-hrtf-cache";
            }
            if (!directory.empty()) {
                settings.hrtf_cache_policy = 2;
                settings.hrtf_cache_dir = directory;
            } else {
                settings.hrtf_cache_policy = 1;
            }
        }
        settings.pipeline_delay_samples = joc_decode::kJocSeekPipelineDelaySamples;
        settings.seek_preroll_frames = joc_decode::kJocSeekPrerollFrames;

        double duration = 0.0;
        if (!probe(path, &settings, &duration)) return false;

        // A container header may not state a duration; the player knows it.
        if (!(duration > 0.0)) {
            double from_player = 0.0;
            if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &from_player) >= 0) {
                duration = from_player;
            }
        }
        if (!(duration > 0.0)) {
            joc_log::line("start: no duration for %s, leaving the input to mpv", path.c_str());
            osd(mpv, "JOC: unknown duration, plugin skipped");
            return false;
        }
        settings.length_seconds = duration;

        auto source = std::make_shared<joc_mpv::Source>();
        std::string error;
        if (!source->open(path, settings, duration, &error)) {
            // A disk cache that cannot be used must not cost playback: retry the
            // way this worked before the cache existed.
            if (settings.hrtf_cache_policy == 2) {
                joc_log::line("prepare: disk HRTF cache unusable (%s); retrying in memory",
                              error.c_str());
                settings.hrtf_cache_policy = 1;
                settings.hrtf_cache_dir.clear();
                error.clear();
            }
            if (!source->open(path, settings, duration, &error)) {
                joc_log::line("start: renderer failed for %s: %s", path.c_str(), error.c_str());
                osd(mpv, "JOC: " + error);
                return false;
            }
        }

        auto created = std::make_shared<Stream>();
        created->source = source;
        created->header = joc_mpv::wav_header(
            source->channels(), source->bits_per_sample(),
            joc_mpv::channel_mask(source->channels()), source->data_bytes());
        created->total_bytes = created->header.size() + source->data_bytes();
        const double per_write = config.chunk_seconds * 48000.0 *
                                 static_cast<double>(source->bytes_per_frame());
        created->write_bytes = static_cast<std::size_t>(
            std::min(4.0 * 1024.0 * 1024.0, std::max(16.0 * 1024.0, per_write)));
        created->verbose = config.verbose;

        auto http = std::make_unique<joc_mpv::HttpServer>();
        unsigned short port = 0;
        const std::shared_ptr<Stream> captured = created;
        if (!http->start(
                [captured](const joc_mpv::HttpRequest& request, joc_mpv::HttpResponse* response) {
                    serve_request(captured, request, response);
                },
                &port, &error)) {
            joc_log::line("start: %s", error.c_str());
            osd(mpv, "JOC: " + error);
            source->close();
            return false;
        }

        stream = created;
        server = std::move(http);
        current_path = path;
        url = "http://127.0.0.1:" + std::to_string(port) + kResource;
        probed_joc = true;
        joc_log::line("prepare: %s -> %s (%s, %s)", path.c_str(), url.c_str(),
                      format_channels(source->channels(), config.layout, config.output).c_str(),
                      config.ffmpeg.c_str());
        return true;
    }

    // Adds the prepared stream as a selected audio track and lets the player's
    // own audio come back.
    void attach() {
        if (!stream || current_path.empty()) return;
        if (!config.attach) {
            joc_log::line("attach: disabled (joc-attach=no); serving %s", url.c_str());
            restore_bed();
            return;
        }
        const std::string title =
            "JOC " + (config.output == 0 ? std::string("binaural") : config.layout);
        const char* arguments[] = {"audio-add", url.c_str(), "select", title.c_str(), nullptr};
        const int result = mpv_command(mpv, arguments);
        if (result < 0) {
            joc_log::line("attach: audio-add failed: %s", mpv_error_string(result));
            osd(mpv, std::string("JOC: audio-add failed: ") + mpv_error_string(result));
            stop_stream();
            restore_bed();
            return;
        }
        restore_bed();
        const std::string summary =
            "JOC: " + format_channels(stream->source->channels(), config.layout, config.output) +
            (config.force_int24 ? " int24" : " float32");
        joc_log::line("attach: %s (%s)", current_path.c_str(), summary.c_str());
        osd(mpv, summary);
    }
};

std::string read_string_property(mpv_handle* mpv, const char* name) {
    char* value = mpv_get_property_string(mpv, name);
    if (value == nullptr) return {};
    std::string text(value);
    mpv_free(value);
    return text;
}

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), needed);
    wide.resize(static_cast<std::size_t>(needed - 1));
    return wide;
}

// Diagnostics must not land in the directory mpv scans for scripts: every file
// there is offered to the script loader.  The player's config directory is the
// natural place; one level above the plugin is the fallback.
void configure_log_directory(mpv_handle* mpv, const joc_mpv::Config& config) {
    std::string path = config.log_path;
    if (path.empty()) {
        std::string directory = read_string_property(mpv, "options/config-dir");
        if (directory.empty()) directory = plugin_directory() + "\\..";
        path = directory + "\\joc_decoder.log";
    }
    const std::wstring wide = to_wide(path);
    if (!wide.empty()) SetEnvironmentVariableW(L"JOC_LOG", wide.c_str());
}

void configure_output(mpv_handle* mpv, const joc_mpv::Config& config) {
    if (config.wasapi == "yes") {
        const int result = mpv_set_property_string(mpv, "ao", "wasapi");
        joc_log::line("output: ao=wasapi -> %s", result < 0 ? mpv_error_string(result) : "ok");
    }
    if (config.exclusive) {
        const int result = mpv_set_property_string(mpv, "audio-exclusive", "yes");
        joc_log::line("output: audio-exclusive=yes -> %s",
                      result < 0 ? mpv_error_string(result) : "ok");
    }
}

}  // namespace

extern "C" __declspec(dllexport) int mpv_open_cplugin(mpv_handle* handle) {
    Plugin plugin;
    plugin.mpv = handle;
    std::string warnings;
    const std::string script_opts = read_string_property(handle, "options/script-opts");
    plugin.config = joc_mpv::parse_config(script_opts, &warnings);
    plugin.config_dir = read_string_property(handle, "options/config-dir");
    configure_log_directory(handle, plugin.config);
    if (!warnings.empty()) joc_log::line("config: %s", warnings.c_str());

    joc_log::line("plugin: joc_mpv loaded (libmpv %lu, dir %s)",
                  static_cast<unsigned long>(mpv_client_api_version()),
                  plugin_directory().c_str());
    configure_output(handle, plugin.config);

    for (;;) {
        mpv_event* event = mpv_wait_event(handle, -1);
        if (event == nullptr) continue;
        switch (event->event_id) {
            case MPV_EVENT_START_FILE: {
                // The renderer starts here, while mpv is still opening the file,
                // and the file's own audio is silenced first so the opening
                // moments are not the plain bed.
                plugin.restore_bed();
                const std::string path = read_string_property(handle, "path");
                if (path.empty() || path.find("://") != std::string::npos) break;
                if (Plugin::candidate(path)) plugin.suppress_bed();
                if (!plugin.prepare(path)) plugin.restore_bed();
                break;
            }
            case MPV_EVENT_FILE_LOADED: {
                const std::string path = read_string_property(handle, "path");
                if (path.empty() || path.find("://") != std::string::npos) break;
                if (plugin.current_path != path) {
                    // No start-file notification (a reload, for instance), or a
                    // file the probe rejected there.
                    if (plugin.probed_path == path && !plugin.probed_joc) {
                        plugin.restore_bed();
                        break;
                    }
                    if (!plugin.prepare(path)) {
                        plugin.restore_bed();
                        break;
                    }
                }
                plugin.attach();
                break;
            }
            case MPV_EVENT_END_FILE:
                plugin.stop_stream();
                plugin.restore_bed();
                break;
            case MPV_EVENT_SHUTDOWN:
                plugin.stop_stream();
                plugin.restore_bed();
                joc_log::line("plugin: shutting down");
                return 0;
            default:
                break;
        }
    }
}
