/*
 * MIT License, Copyright (c) 2026 TheM14
 *
 * Derived from the upstream project's eac3_scan.cpp (JustOneCacophony),
 * rewritten in C.
 * See LICENSE-MIT.
 */

#include <string.h>

#include "audio/decode/joc_eac3.h"

#define EAC3_SYNCWORD 0x0B77u

static uint32_t read_bits(const uint8_t *data, size_t start, size_t count)
{
    uint32_t value = 0;
    for (size_t index = start; index < start + count; ++index)
        value = (value << 1) | ((data[index >> 3] >> (7u - (index & 7u))) & 1u);
    return value;
}

/* AC-3 syncframe length in 16-bit words, indexed by fscod then frmsizecod. */
static const uint16_t ac3_frame_words[3][38] = {
    {64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160, 192, 192, 224, 224,
     256, 256, 320, 320, 384, 384, 448, 448, 512, 512, 640, 640, 768, 768,
     896, 896, 1024, 1024, 1152, 1152, 1280, 1280},
    {69, 70, 87, 88, 104, 105, 121, 122, 139, 140, 174, 175, 208, 209, 243, 244,
     278, 278, 348, 348, 417, 418, 487, 487, 557, 557, 696, 696, 835, 835,
     975, 975, 1114, 1114, 1253, 1253, 1393, 1393},
    {96, 96, 120, 120, 144, 144, 168, 168, 192, 192, 240, 240, 288, 288, 336, 336,
     384, 384, 480, 480, 576, 576, 672, 672, 768, 768, 960, 960, 1152, 1152,
     1344, 1344, 1536, 1536, 1728, 1728, 1920, 1920},
};

size_t joc_eac3_frame_header_at(const uint8_t *data, size_t size, size_t offset,
                                struct joc_eac3_header *out)
{
    if (!data || offset + 6u > size)
        return 0;
    uint16_t syncword = (uint16_t)((data[offset] << 8) | data[offset + 1]);
    if (syncword != EAC3_SYNCWORD)
        return 0;
    size_t base = offset * 8u;
    uint32_t bsid = read_bits(data, base + 40u, 5u);
    if (bsid <= 8u) {
        uint32_t fscod = read_bits(data, base + 32u, 2u);
        uint32_t frmsizecod = read_bits(data, base + 34u, 6u);
        if (fscod > 2u || frmsizecod >= 38u)
            return 0;
        size_t bytes = (size_t)ac3_frame_words[fscod][frmsizecod] * 2u;
        if (out) {
            out->frame_bytes = bytes;
            out->offset = offset;
            out->codec = 0;
            out->strmtyp = 0;
            out->substreamid = 0;
        }
        return bytes;
    }
    size_t words = ((size_t)(data[offset + 2] & 0x07u) << 8) | data[offset + 3];
    size_t bytes = (words + 1u) * 2u;
    if (out) {
        out->frame_bytes = bytes;
        out->offset = offset;
        out->codec = 1;
        out->strmtyp = (uint8_t)read_bits(data, base + 16u, 2u);
        out->substreamid = (uint8_t)read_bits(data, base + 18u, 3u);
    }
    return bytes;
}

size_t joc_eac3_audio_frame_bytes(const uint8_t *data, size_t size, size_t offset,
                                  int end_of_stream)
{
    struct joc_eac3_header seen[8];
    size_t count = 0;
    size_t consumed = 0;
    while (count < 8) {
        struct joc_eac3_header header;
        size_t bytes = joc_eac3_frame_header_at(data, size, offset + consumed, &header);
        if (bytes == 0 || offset + consumed + bytes > size)
            break;
        int repeats = 0;
        for (size_t index = 0; index < count; ++index) {
            if (seen[index].codec == header.codec && seen[index].strmtyp == header.strmtyp &&
                seen[index].substreamid == header.substreamid) {
                repeats = 1;
                break;
            }
        }
        if (repeats)
            return consumed;
        seen[count++] = header;
        consumed += bytes;
    }
    if (count != 0u && consumed != 0u && end_of_stream)
        return consumed;
    return 0;
}
