/*
 * MIT License, Copyright (c) 2026 TheM14
 *
 * Derived from the upstream project's eac3_scan.cpp (JustOneCacophony),
 * rewritten in C.
 * See LICENSE-MIT.
 */

/* E-AC-3 syncframe walking, sized for the audio decoder in ad_joc.c.
 *
 * An E-AC-3 "audio frame" can be more than one syncframe: a 7-channel JOC
 * program interleaves an independent and a dependent substream, and the
 * renderer pairs one whole audio frame with one 1536-sample bed frame.  These
 * helpers are a C port of the subset of the upstream eac3_scan.cpp that the
 * decoder needs (no EMDF/JOC inspection: the renderer itself validates that).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct joc_eac3_header {
    size_t frame_bytes;
    size_t offset;
    uint8_t codec;        /* 0 = AC-3, 1 = E-AC-3 */
    uint8_t strmtyp;
    uint8_t substreamid;
};

/* Length of the syncframe at data[offset], or 0 when there is no valid header.
 * AC-3 and E-AC-3 are told apart by bsid, exactly as the renderer does. */
size_t joc_eac3_frame_header_at(const uint8_t *data, size_t size, size_t offset,
                                struct joc_eac3_header *out);

/* Length of the whole audio frame at data[offset]: every syncframe up to the
 * first repeated (codec, strmtyp, substreamid).  Returns 0 while the group is
 * still open inside the buffer, unless end_of_stream says no more bytes will
 * arrive. */
size_t joc_eac3_audio_frame_bytes(const uint8_t *data, size_t size, size_t offset,
                                  int end_of_stream);
