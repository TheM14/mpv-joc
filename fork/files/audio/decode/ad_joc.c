/*
 * MIT License, Copyright (c) 2026 TheM14
 *
 * This file is compiled into mpv, so the resulting player is distributed under
 * mpv's own license (LGPL-2.1-or-later, or GPL-2.0-or-later when built with
 * -Dgpl=true).  mpv's license text is in LICENSE-mpv.
 */

/*
 * ad_joc.c — mpv audio decoder for E-AC-3 JOC, rendered by the
 * JustOneCacophony core (joc_core.dll).
 *
 * Opt in with --ad=joc.  The demuxer hands us E-AC-3 access units; we decode
 * the bed with libavcodec (drc off, as the reference frontends do), and push
 * each whole audio frame plus its bed PCM into joc_stream.  What comes back is
 * the object-audio rendering (binaural or a speaker layout) as ordinary float
 * PCM, so mpv's resampler, filters, A/V sync and audio output apply unchanged.
 *
 * This is the "native track" integration: no second audio track, no loopback
 * endpoint, correct codec information in the track list.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

#include "config.h"

#include "mpv_talloc.h"
#include "osdep/timer.h"

#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"
#include "options/m_config.h"
#include "options/options.h"
#include "options/path.h"
#include "osdep/io.h"

#include "common/codecs.h"
#include "common/common.h"

#include "audio/decode/ad_joc.h"
#include "audio/decode/joc_eac3.h"
#include "audio/decode/joc_stream.h"

#define OPT_BASE_STRUCT struct ad_joc_params

const struct m_sub_options ad_joc_conf = {
    .opts = (const m_option_t[]) {
        {"binaural", OPT_BOOL(binaural)},
        {"hrtf", OPT_STRING(hrtf), .flags = M_OPT_FILE},
        {"layout", OPT_STRING(layout), .flags = M_OPT_FILE},
        {"mode", OPT_CHOICE(binaural_mode,
                            {"near", 1}, {"far", 2}, {"mid", 3})},
        {"gain-db", OPT_DOUBLE(gain_db), M_RANGE(-60.0, 12.0)},
        {"int24", OPT_BOOL(int24)},
        {0}
    },
    .size = sizeof(struct ad_joc_params),
    .defaults = &(const struct ad_joc_params){
        .binaural = true,
        .int24 = false,
        .hrtf = "",
        .layout = "7.1",
        .binaural_mode = 3,
        .gain_db = 0.0,
    },
};

#define JOC_FRAME_SAMPLES 1536
#define PULL_BLOCK 4096

struct priv {
    struct mp_decoder public;
    AVCodecContext *avctx;
    AVFrame *avframe;
    AVPacket *avpkt;
    joc_stream *stream;
    joc_stream_config cfg;
    int out_channels;
    struct mp_aframe *fmt;
    struct mp_aframe_pool *pool;
    uint8_t *carry;
    int carry_len;
    int carry_cap;
    float *bed;
    size_t bed_cap;          /* floats */
    int bed_channels;
    uint64_t frames_in;      /* audio frames pushed to the renderer */
    uint64_t samples_out;    /* samples handed to mpv */
    uint64_t last_report;    /* heartbeat bookkeeping */
    double last_stall_log;   /* rate limits the "no output" warning */
    double next_pts;
    bool have_pts;
    bool eof;
    bool binaural;
    bool int24;
    enum { PATH_UNDECIDED, PATH_JOC, PATH_NATIVE } path;
    struct mp_decoder *native;
    struct mp_codec_params *codec;
    struct mp_frame *probe_packets;
    int num_probe_packets;
    int probe_packet_pos;
    struct mp_frame eof_frame;
    bool eof_pending;
};

static int append(struct priv *p, const uint8_t *data, int len)
{
    if (len <= 0)
        return 0;
    if (p->carry_len + len > p->carry_cap) {
        int cap = p->carry_cap ? p->carry_cap : 65536;
        while (cap < p->carry_len + len)
            cap *= 2;
        p->carry = talloc_realloc(p, p->carry, uint8_t, cap);
        p->carry_cap = cap;
    }
    memcpy(p->carry + p->carry_len, data, len);
    p->carry_len += len;
    return len;
}

static void push_bed(struct priv *p, const uint8_t *data, int bytes)
{
    av_packet_unref(p->avpkt);
    if (av_new_packet(p->avpkt, bytes) < 0)
        return;
    memcpy(p->avpkt->data, data, bytes);

    if (avcodec_send_packet(p->avctx, p->avpkt) < 0)
        return;
    while (avcodec_receive_frame(p->avctx, p->avframe) == 0) {
        int channels = p->avframe->ch_layout.nb_channels;
        int samples = p->avframe->nb_samples;
        if (channels <= 0 || samples <= 0) {
            av_frame_unref(p->avframe);
            continue;
        }
        if (p->bed_channels && p->bed_channels != channels) {
            /* The core pairs one audio frame with one bed frame of the channel
             * count it derived; a mid-stream change cannot be paired. */
            av_frame_unref(p->avframe);
            continue;
        }
        if (!p->bed_channels) {
            p->bed_channels = channels;
            MP_VERBOSE(p->public.f, "bed: %d channel(s), %d samples/frame\n",
                       channels, samples);
        }
        size_t need = (size_t)samples * channels;
        if (need > p->bed_cap) {
            p->bed = talloc_realloc(p, p->bed, float, need);
            p->bed_cap = need;
        }
        for (int s = 0; s < samples; ++s) {
            for (int c = 0; c < channels; ++c) {
                const float *plane = (const float *)p->avframe->extended_data[c];
                p->bed[(size_t)s * channels + c] = plane[s];
            }
        }
        joc_stream_buffer in;
        memset(&in, 0, sizeof(in));
        in.struct_size = sizeof(in);
        in.struct_version = 1;
        in.kind = JOC_STREAM_IN_CORE_PCM;
        in.channels = (uint32_t)channels;
        in.sample_rate = 48000;
        in.sample_count = (uint32_t)samples;
        in.pcm = p->bed;
        joc_stream_push(p->stream, &in, NULL, NULL);
        av_frame_unref(p->avframe);
    }
}

static void feed(struct priv *p, int end_of_stream)
{
    if (!p->stream)
        return;
    for (;;) {
        size_t bytes = joc_eac3_audio_frame_bytes(p->carry, (size_t)p->carry_len, 0,
                                                  end_of_stream);
        if (bytes == 0 || (int)bytes > p->carry_len)
            break;
        joc_stream_buffer in;
        memset(&in, 0, sizeof(in));
        in.struct_size = sizeof(in);
        in.struct_version = 1;
        in.kind = JOC_STREAM_IN_EAC3;
        in.bytes = p->carry;
        in.byte_count = (uint32_t)bytes;
        uint32_t consumed = 0;
        if (joc_stream_push(p->stream, &in, NULL, &consumed) != JOC_OK) {
            MP_ERR(p->public.f, "renderer refused an E-AC-3 frame (%u bytes): %s\n",
                   bytes, joc_last_error_detail());
            break;
        }
        p->frames_in++;
        push_bed(p, p->carry, (int)bytes);
        int left = p->carry_len - (int)bytes;
        if (left > 0)
            memmove(p->carry, p->carry + bytes, (size_t)left);
        p->carry_len = left;
    }
}

static float quantize_int24(float v);

static bool emit(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!p->stream || !mp_pin_in_needs_data(f->ppins[1]))
        return false;

    struct mp_aframe *frame = mp_aframe_new_ref(p->fmt);
    if (!frame)
        return false;
    if (mp_aframe_pool_allocate(p->pool, frame, PULL_BLOCK) < 0) {
        talloc_free(frame);
        return false;
    }
    uint8_t **data = mp_aframe_get_data_rw(frame);
    if (!data) {
        talloc_free(frame);
        return false;
    }

    joc_stream_buffer out;
    memset(&out, 0, sizeof(out));
    out.struct_size = sizeof(out);
    out.struct_version = 1;
    out.kind = p->binaural ? JOC_STREAM_OUT_BINAURAL : JOC_STREAM_OUT_SPEAKER;
    out.channels = (uint32_t)p->out_channels;
    out.sample_rate = 48000;
    out.sample_count = PULL_BLOCK;
    out.out_pcm = (float *)data[0];
    uint32_t produced = 0;
    joc_error rc = joc_stream_pull(p->stream, &out, &produced);
    if (rc != JOC_OK) {
        MP_ERR(f, "renderer pull failed: %s\n", joc_last_error_detail());
        talloc_free(frame);
        return false;
    }
    if (produced == 0) {
        const double now = mp_time_sec();
        joc_stream_status_info st = { .struct_size = sizeof(st), .struct_version = 1 };
        joc_stream_status(p->stream, &st);
        /* A frame still waiting in carry is normal: the renderer pulls only
         * what it has already rendered.  Only a dry renderer with nothing in
         * flight means the pipeline actually stopped. */
        if (p->carry_len == 0 && st.buffered_samples == 0 &&
            now - p->last_stall_log > 1.0) {
            p->last_stall_log = now;
            MP_WARN(f, "no output: frames_in=%llu frames_out=%llu samples_out=%llu "
                       "buffered=%llu carry=%d eof=%d state=%d\n",
                    (unsigned long long)st.frames_in,
                    (unsigned long long)st.frames_out,
                    (unsigned long long)p->samples_out,
                    (unsigned long long)st.buffered_samples,
                    p->carry_len, p->eof, (int)p->path);
        }
        talloc_free(frame);
        return false;
    }

    /* The renderer's room tail is not part of the file: stop at the sample the
     * last input frame ends on, exactly as the file frontends do. */
    uint64_t total = p->frames_in * JOC_FRAME_SAMPLES;
    uint64_t left = (p->samples_out < total) ? total - p->samples_out : 0;
    if ((uint64_t)produced > left)
        produced = (uint32_t)left;
    if (produced == 0) {
        talloc_free(frame);
        return false;
    }
    if (p->int24) {
        float *s = (float *)data[0];
        for (size_t i = 0; i < (size_t)produced * (size_t)p->out_channels; i++)
            s[i] = quantize_int24(s[i]);
    }
    mp_aframe_set_size(frame, (int)produced);
    mp_aframe_set_pts(frame, p->next_pts);
    p->next_pts += (double)produced / 48000.0;
    p->samples_out += produced;
    if (p->frames_in != p->last_report && p->frames_in % 128 == 0) {
        p->last_report = p->frames_in;
        joc_stream_status_info st = { .struct_size = sizeof(st), .struct_version = 1 };
        joc_stream_status(p->stream, &st);
        MP_VERBOSE(f, "frames_in=%llu samples_out=%llu buffered=%llu\n",
                   (unsigned long long)p->frames_in,
                   (unsigned long long)p->samples_out,
                   (unsigned long long)st.buffered_samples);
    }
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_AUDIO, frame));
    return true;
}

static void clear_probe_packets(struct priv *p)
{
    for (int i = p->probe_packet_pos; i < p->num_probe_packets; i++)
        mp_frame_unref(&p->probe_packets[i]);
    p->num_probe_packets = 0;
    p->probe_packet_pos = 0;
}

/* The track is not JOC: hand it to mpv's normal E-AC-3 decoder and replay the
 * packets that were kept while classifying, so the track still plays (and a
 * seek that happened first is transparent). */
static bool switch_to_native(struct mp_filter *f, struct priv *p)
{
    if (p->native)
        return true;

    struct mp_decoder_list *all = talloc_zero(NULL, struct mp_decoder_list);
    ad_lavc.add_decoders(all);
    struct mp_decoder_list *sel =
        mp_select_decoders(f->log, all, p->codec->codec, NULL);
    talloc_free(all);
    for (int i = 0; i < sel->num_entries && !p->native; i++)
        p->native = ad_lavc.create(f, p->codec, sel->entries[i].decoder);
    talloc_free(sel);
    if (!p->native) {
        MP_ERR(f, "no native E-AC-3 decoder to fall back to\n");
        return false;
    }

    MP_WARN(f, "this E-AC-3 stream carries no JOC metadata; decoding it with "
               "the default decoder instead\n");
    if (p->stream) {
        joc_stream_destroy(p->stream);
        p->stream = NULL;
    }
    p->path = PATH_NATIVE;
    return true;
}

/* Host mode: pump the native child — packets in, decoded frames out. */
static void process_host(struct mp_filter *f, struct priv *p)
{
    if (!switch_to_native(f, p)) {
        mp_filter_internal_mark_failed(f);
        return;
    }

    struct mp_pin *child_in = p->native->f->pins[0];
    struct mp_pin *child_out = p->native->f->pins[1];

    if (mp_pin_can_transfer_data(f->ppins[1], child_out)) {
        struct mp_frame frame = mp_pin_out_read(child_out);
        mp_pin_in_write(f->ppins[1], frame);
    }
    if (p->probe_packet_pos < p->num_probe_packets &&
        mp_pin_in_needs_data(child_in)) {
        struct mp_frame frame = p->probe_packets[p->probe_packet_pos];
        p->probe_packets[p->probe_packet_pos++] = MP_NO_FRAME;
        mp_pin_in_write(child_in, frame);
        if (p->probe_packet_pos == p->num_probe_packets) {
            p->num_probe_packets = 0;
            p->probe_packet_pos = 0;
        }
    } else if (mp_pin_can_transfer_data(child_in, f->ppins[0])) {
        struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
        mp_pin_in_write(child_in, frame);
    }
}

/* True when the renderer accepted these frames as JOC. A stream without any
 * EMDF metadata never produces output and counts every frame as missing. */
static bool classify_has_joc(struct priv *p)
{
    joc_stream_status_info st;
    memset(&st, 0, sizeof(st));
    st.struct_size = sizeof(st);
    st.struct_version = 1;
    if (joc_stream_status(p->stream, &st) != JOC_OK)
        return true;
    return st.frames_out > 0;
}

static void ad_joc_process(struct mp_filter *f)
{
    struct priv *p = f->priv;

    if (p->path == PATH_NATIVE) {
        process_host(f, p);
        return;
    }
    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    /* End of stream: the renderer still holds its tail, and the EOF frame is
     * only forwarded once that tail has been handed out.  Draining has to
     * happen before touching the input pin, which stays empty from here on. */
    if (p->eof_pending) {
        if (emit(f))
            return;
        struct mp_frame eof = p->eof_frame;
        p->eof_pending = false;
        p->eof_frame = MP_NO_FRAME;
        mp_pin_in_write(f->ppins[1], eof);
        return;
    }

    struct mp_frame in = mp_pin_out_read(f->ppins[0]);
    if (in.type == MP_FRAME_EOF) {
        if (p->path == PATH_UNDECIDED && p->frames_in > 0) {
            if (classify_has_joc(p)) {
                p->path = PATH_JOC;
                clear_probe_packets(p);
            } else if (switch_to_native(f, p)) {
                struct mp_pin *child_in = p->native->f->pins[0];
                for (int i = 0; i < p->num_probe_packets; i++) {
                    if (!mp_pin_in_needs_data(child_in))
                        break;
                    struct mp_frame pf = p->probe_packets[i];
                    p->probe_packets[i] = MP_NO_FRAME;
                    mp_pin_in_write(child_in, pf);
                }
                clear_probe_packets(p);
                if (mp_pin_in_needs_data(child_in))
                    mp_pin_in_write(child_in, in);
                else
                    mp_frame_unref(&in);
                process_host(f, p);
                mp_filter_internal_mark_progress(f);
                return;
            }
        }
        if (!p->eof) {
            p->eof = true;
            if (p->carry_len > 0) {
                feed(p, 1);
                p->carry_len = 0;
            }
            joc_stream_flush(p->stream);
        }
        p->eof_frame = in;
        p->eof_pending = true;
        if (emit(f))
            return;
        /* Nothing buffered: forward EOF immediately. */
        struct mp_frame eof = p->eof_frame;
        p->eof_pending = false;
        p->eof_frame = MP_NO_FRAME;
        mp_pin_in_write(f->ppins[1], eof);
        return;
    }
    if (in.type != MP_FRAME_PACKET) {
        if (in.type)
            MP_ERR(f, "unexpected frame type\n");
        return;
    }

    struct demux_packet *pkt = in.data;
    if (!p->have_pts && pkt->pts != MP_NOPTS_VALUE) {
        p->next_pts = pkt->pts;
        p->have_pts = true;
    }
    /* Keep what was read while classifying the track: if it turns out to have
     * no JOC payload, these packets are replayed into the native decoder. */
    bool retained = false;
    if (p->path == PATH_UNDECIDED && p->num_probe_packets < 16) {
        MP_TARRAY_APPEND(p, p->probe_packets, p->num_probe_packets, in);
        retained = true;
    }
    append(p, pkt->buffer, pkt->len);
    feed(p, 0);
    if (!retained)
        talloc_free(pkt);

    if (p->path == PATH_UNDECIDED && p->frames_in >= 8) {
        if (classify_has_joc(p)) {
            p->path = PATH_JOC;
            clear_probe_packets(p);
        } else {
            process_host(f, p);
            return;
        }
    }

    if (emit(f))
        return;
    mp_filter_internal_mark_progress(f);
}

static void ad_joc_reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->path == PATH_UNDECIDED)
        clear_probe_packets(p);
    /* mpv restarts the packet stream at the seek target without pre-roll, so
     * the renderer cannot be carried across the discontinuity: whatever it
     * buffered belongs to the old position and would be emitted as if it were
     * the new one.  Restart it instead; the missing history costs a short
     * convergence, the stale tail would be audible for about a second. */
    if (p->stream) {
        joc_stream_destroy(p->stream);
        p->stream = NULL;
        if (joc_stream_create(&p->cfg, &p->stream) != JOC_OK) {
            MP_ERR(f, "cannot restart the renderer after a seek: %s\n",
                   joc_last_error_detail());
            p->stream = NULL;
        }
    }
    p->frames_in = 0;
    p->samples_out = 0;
    if (p->avctx)
        avcodec_flush_buffers(p->avctx);
    if (p->path == PATH_NATIVE)
        return;
    p->carry_len = 0;
    p->have_pts = false;
    p->eof = false;
    p->bed_channels = 0;
}

static void ad_joc_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->stream) {
        joc_stream_destroy(p->stream);
        p->stream = NULL;
    }
    if (p->avctx)
        avcodec_free_context(&p->avctx);
    av_frame_free(&p->avframe);
    av_packet_free(&p->avpkt);
}

static const struct mp_filter_info ad_joc_filter = {
    .name = "ad_joc",
    .priv_size = sizeof(struct priv),
    .process = ad_joc_process,
    .reset = ad_joc_reset,
    .destroy = ad_joc_destroy,
};

/* The layouts the core renders, with the channel order it writes them in, spelled
 * with mpv's speaker names.  The order is per channel and is not the order of a
 * WAVE channel mask: 7.1 and up write the side pair before the rear pair, so mpv
 * names up to 7.1 are not used as they are.  The core's 4.0 is the quad layout
 * (front and back pair). */
static const struct {
    const char *name;
    const char *speakers;
} layout_names[] = {
    {"2.0",   "fl-fr"},
    {"3.1",   "fl-fr-fc-lfe"},
    {"4.0",   "fl-fr-bl-br"},
    {"5.1",   "fl-fr-fc-lfe-bl-br"},
    {"7.1",   "fl-fr-fc-lfe-sl-sr-bl-br"},
    {"5.1.2", "fl-fr-fc-lfe-bl-br-tsl-tsr"},
    {"5.1.4", "fl-fr-fc-lfe-bl-br-tfl-tfr-tbl-tbr"},
    {"7.1.2", "fl-fr-fc-lfe-sl-sr-bl-br-tsl-tsr"},
    {"7.1.4", "fl-fr-fc-lfe-sl-sr-bl-br-tfl-tfr-tbl-tbr"},
    {"9.1.4", "fl-fr-fc-lfe-sl-sr-bl-br-wl-wr-tfl-tfr-tbl-tbr"},
    {"9.1.6", "fl-fr-fc-lfe-sl-sr-bl-br-wl-wr-tfl-tfr-tsl-tsr-tbl-tbr"},
};

static bool layout_chmap(struct mp_chmap *chmap, const char *layout, int channels)
{
    for (int i = 0; i < MP_ARRAY_SIZE(layout_names); i++) {
        if (strcmp(layout, layout_names[i].name) == 0) {
            if (mp_chmap_from_str(chmap, bstr0(layout_names[i].speakers)) &&
                chmap->num == channels)
                return true;
            break;
        }
    }
    if (mp_chmap_from_str(chmap, bstr0(layout)) && chmap->num == channels)
        return true;
    /* Unknown to mpv: keep the channels, just do not name them. */
    mp_chmap_from_channels(chmap, channels);
    return chmap->num == channels;
}

/* The renderer's int24 rule (the core's wav_writer to_int32):
 * clip, scale by 8388607, truncate toward zero, keep the low 24 bits, and read
 * back the value the host sees: n / 2^23. */
static float quantize_int24(float v)
{
    if (!isfinite(v))
        return 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    else if (v < -1.0f)
        v = -1.0f;
    int32_t scaled = (int32_t)(v * 8388607.0f);
    int32_t written = scaled & 0xFFFFFF;
    if (written & 0x800000)
        written -= 0x1000000;
    return (float)written / 8388608.0f;
}

enum hrtf_kind { HRTF_UNKNOWN, HRTF_SOFA, HRTF_ROSELLA, HRTF_KERNELS };

static bool file_exists(const char *path)
{
    if (!path || !path[0])
        return false;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    fclose(fp);
    return true;
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b)
        return false;
    for (size_t i = 0; i < b; i++) {
        char x = s[a - b + i], y = suffix[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return false;
    }
    return true;
}

static bool contains(const void *hay, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    if (n < m)
        return false;
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp((const unsigned char *)hay + i, needle, m) == 0)
            return true;
    }
    return false;
}

/* The renderer takes three kinds of HRTF input and the user should not have to
 * say which: SOFA is HDF5, the Rosella model is JSON with a personalized_hrtf
 * object, and a compiled kernel is a .jochrtf.  Look at the extension first
 * (conclusive for kernels), then at the file signature. */
static enum hrtf_kind hrtf_kind_of(const char *path)
{
    if (!path || !path[0])
        return HRTF_UNKNOWN;
    if (ends_with_ci(path, ".jochrtf"))
        return HRTF_KERNELS;

    unsigned char head[4096];
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return HRTF_UNKNOWN;
    size_t n = fread(head, 1, sizeof(head), fp);
    fclose(fp);

    if (n >= 8 && memcmp(head, "\x89HDF\r\n\x1a\n", 8) == 0)
        return HRTF_SOFA;
    for (size_t i = 0; i < n; i++) {
        if (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n')
            continue;
        if (head[i] == '{' && contains(head, n, "\"personalized_hrtf\""))
            return HRTF_ROSELLA;
        break;
    }
    if (ends_with_ci(path, ".sofa"))
        return HRTF_SOFA;
    return HRTF_UNKNOWN;
}

static char *exe_dir(void *ta)
{
#ifdef _WIN32
    wchar_t wpath[MP_PATH_MAX];
    DWORD len = GetModuleFileNameW(NULL, wpath, MP_PATH_MAX);
    while (len > 0 && wpath[len - 1] != L'\\' && wpath[len - 1] != L'/')
        len--;
    if (len > 0)
        wpath[len - 1] = 0;
    return mp_to_utf8(ta, wpath);
#else
    return NULL;
#endif
}

/* Explicit --ad-joc-hrtf wins; otherwise look where the standalone renderer
 * looks: HRTF/binaural.sofa or HRTF/binaural.personalized_headphone next to
 * mpv.exe, then the same names under the mpv config directory. */
static char *find_hrtf(struct mp_filter *da, struct ad_joc_params *opts)
{
    struct priv *p = da->priv;
    if (opts->hrtf && opts->hrtf[0]) {
        char *path = mp_get_user_path(p, da->global, opts->hrtf);
        if (file_exists(path))
            return path;
        MP_ERR(da, "HRTF file not found: %s\n", path);
        return NULL;
    }

    static const char *const names[] = {
        "HRTF/binaural.sofa",
        "HRTF/binaural.personalized_headphone",
        "HRTF/binaural.jochrtf",
        "joc-hrtf/binaural.sofa",
        "joc-hrtf/binaural.personalized_headphone",
    };
    char *dir = exe_dir(p);
    for (int i = 0; i < MP_ARRAY_SIZE(names); i++) {
        char *cand = dir ? mp_path_join(p, dir, names[i]) : NULL;
        if (file_exists(cand))
            return cand;
        cand = mp_find_config_file(p, da->global, names[i]);
        if (file_exists(cand))
            return cand;
    }
    return NULL;
}

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    if (!codec->codec || strcmp(codec->codec, "eac3") != 0)
        return NULL;

    struct mp_filter *da = mp_filter_create(parent, &ad_joc_filter);
    if (!da)
        return NULL;
    mp_filter_add_pin(da, MP_PIN_IN, "in");
    mp_filter_add_pin(da, MP_PIN_OUT, "out");
    da->log = mp_log_new(da, parent->log, NULL);

    (void)decoder;
    struct priv *p = da->priv;
    struct ad_joc_params *opts = mp_get_config_group(p, da->global, &ad_joc_conf);
    bool binaural = opts->binaural;
    /* The layout name the core is asked for. */
    const char *layout = opts->layout;
    char *hrtf_path = NULL;
    enum hrtf_kind hrtf_kind = HRTF_UNKNOWN;
    if (binaural) {
        hrtf_path = find_hrtf(da, opts);
        hrtf_kind = hrtf_kind_of(hrtf_path);
        if (hrtf_kind == HRTF_UNKNOWN) {
            MP_WARN(da, "no usable HRTF found%s%s; rendering the speaker layout "
                        "instead (mpv downmixes it to stereo). Place "
                        "HRTF/binaural.sofa next to mpv.exe or set --ad-joc-hrtf.\n",
                    hrtf_path ? ": " : "", hrtf_path ? hrtf_path : "");
            binaural = false;
        }
    }

    const AVCodec *lavc = avcodec_find_decoder(AV_CODEC_ID_EAC3);
    if (!lavc) {
        MP_ERR(da, "libavcodec has no E-AC-3 decoder for the bed\n");
        talloc_free(da);
        return NULL;
    }
    p->avctx = avcodec_alloc_context3(lavc);
    p->avframe = av_frame_alloc();
    p->avpkt = av_packet_alloc();
    if (!p->avctx || !p->avframe || !p->avpkt) {
        talloc_free(da);
        return NULL;
    }
    p->avctx->pkt_timebase = (AVRational){1, 48000};
    /* The reference frontends decode the bed without applying the stream's
     * dynamic range metadata. */
    av_opt_set_double(p->avctx->priv_data, "drc_scale", 0.0, 0);
    if (avcodec_open2(p->avctx, lavc, NULL) < 0) {
        MP_ERR(da, "cannot open the E-AC-3 bed decoder\n");
        talloc_free(da);
        return NULL;
    }

    joc_stream_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.struct_version = 1;
    cfg.input = JOC_STREAM_IN_EAC3;
    cfg.output = binaural ? JOC_STREAM_OUT_BINAURAL : JOC_STREAM_OUT_SPEAKER;
    cfg.binaural_mode = (uint32_t)opts->binaural_mode;
    cfg.gain_db = opts->gain_db;
    cfg.object_delay_samples = 1473;
    if (binaural) {
        if (hrtf_kind == HRTF_SOFA)
            cfg.hrtf_sofa_path = hrtf_path;
        else if (hrtf_kind == HRTF_ROSELLA)
            cfg.personalized_headphone_path = hrtf_path;
        else if (hrtf_kind == HRTF_KERNELS)
            cfg.hrtf_path = hrtf_path;
        /* Compiling a SOFA is expensive; keep the kernels between runs. */
        char *cache = mp_find_user_file(p, da->global, "cache", "joc-hrtf-cache");
        if (!cache) {
            char *dir = exe_dir(p);
            if (dir)
                cache = mp_path_join(p, dir, "joc-hrtf-cache");
        }
        if (cache) {
            mp_mkdirp(cache);
            cfg.hrtf_cache_dir = cache;
            cfg.hrtf_cache_policy = 2;
        } else {
            cfg.hrtf_cache_policy = 1;
        }
    } else {
        cfg.speaker_layout_name = layout;
    }
    p->cfg = cfg;
    if (joc_stream_create(&cfg, &p->stream) != JOC_OK) {
        MP_ERR(da, "cannot create the renderer stream: %s\n", joc_last_error_detail());
        talloc_free(da);
        return NULL;
    }

    joc_stream_status_info status;
    memset(&status, 0, sizeof(status));
    status.struct_size = sizeof(status);
    status.struct_version = 1;
    int channels = binaural ? 2 : 0;
    if (joc_stream_status(p->stream, &status) == JOC_OK && status.output_channels)
        channels = (int)status.output_channels;
    if (channels <= 0)
        channels = binaural ? 2 : 8;

    struct mp_chmap chmap;
    if (binaural) {
        mp_chmap_from_channels(&chmap, 2);
    } else if (!layout_chmap(&chmap, layout, channels)) {
        MP_ERR(da, "unknown speaker layout '%s'\n", layout);
        talloc_free(da);
        return NULL;
    }
    p->out_channels = channels;

    p->fmt = mp_aframe_create();
    mp_aframe_set_format(p->fmt, AF_FORMAT_FLOAT);
    mp_aframe_set_chmap(p->fmt, &chmap);
    mp_aframe_set_rate(p->fmt, 48000);
    p->pool = mp_aframe_pool_create(p);

    p->public.f = da;
    p->binaural = binaural;
    p->codec = codec;
    p->path = PATH_UNDECIDED;
    p->int24 = opts->int24;
    codec->codec_desc = "E-AC-3 JOC (joc_core renderer)";
    MP_VERBOSE(da, "renderer: %s, %d channel(s)%s%s\n",
               binaural ? "binaural" : layout, p->out_channels,
               binaural ? ", hrtf: " : "", binaural ? hrtf_path : "");
    if (p->int24)
        MP_INFO(da, "int24: quantising the output to the renderer's 24-bit rule\n");
    return &p->public;
}

static void add_decoders(struct mp_decoder_list *list)
{
    mp_add_decoder(list, "eac3", "joc",
                   "E-AC-3 JOC object renderer (joc_core)");
}

const struct mp_decoder_fns ad_joc = {
    .create = create,
    .add_decoders = add_decoders,
};
