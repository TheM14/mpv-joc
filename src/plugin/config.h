#pragma once

#include <string>

namespace joc_mpv {

// Everything the plugin can be told through mpv's own configuration file:
// script-opts keys are "joc-<name>=<value>" (see config/mpv.conf.example).
struct Config {
    bool enabled = true;
    // Take over ordinary PCM sources and render them binaurally through ffmpeg.
    bool pcm = false;
    // 0 = binaural (2 channels), 1 = speaker layout (N channels).
    int output = 0;
    std::string layout = "7.1";
    // Rosella filterbank: 0 = the renderer's default (public bank), 2 = captured.
    int rosella_filterbank = 0;
    std::string hrtf_file;          // empty: <plugin dir>\\HRTF\\binaural.<ext>
    // 1 = near, 2 = far, 3 = mid (the renderer's own default).
    int binaural_mode = 3;
    double gain_db = 0.0;
    double tail_seconds = 5.0;
    bool force_int24 = false;
    unsigned object_delay_samples = 1473;
    unsigned native_threads = 0;
    std::string ffmpeg = "ffmpeg";
    std::string core_dll;           // empty: <plugin dir>\\joc_core.dll
    // auto: leave mpv's audio output alone; yes: force the WASAPI AO.
    std::string wasapi = "auto";
    bool exclusive = false;
    bool verbose = false;
    // no = serve the rendered stream without adding it to the player; used to
    // capture exactly what the plugin would deliver, and for debugging.
    bool attach = true;
    // Silence the file's own audio track until the rendered one is selected, so
    // the opening moments are not the plain bed.  Only candidate inputs are
    // muted, and only for as long as the probe takes.
    bool mute_until_ready = true;
    // Log file path; empty means <mpv config dir>\joc_decoder.log.
    std::string log_path;
    // Samples requested per HTTP response, in seconds of audio.  A bounded
    // response makes mpv come back with a fresh range request after a seek
    // instead of reading through everything in between.
    double chunk_seconds = 2.0;
};

// Parses mpv's script-opts value ("joc-output=binaural,joc-hrtf=..."); unknown
// keys are ignored so an mpv.conf written for a newer build still loads.
Config parse_config(const std::string& script_opts, std::string* warnings);

bool valid_layout(const std::string& layout);

}  // namespace joc_mpv
