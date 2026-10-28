#pragma once

#include <cstdint>
#include <string>

namespace joc_mpv {

// Channel mask for the served WAV.  The PCM follows the renderer's channel order,
// which no WAVE mask describes, so anything wider than stereo is declared with no
// speaker positions at all (mask 0).  Two channels are FL/FR in either definition.
std::uint32_t channel_mask(unsigned channels);

// 68-byte WAVE_FORMAT_EXTENSIBLE header, or a 104-byte RF64 header when the data
// chunk would not fit a 32-bit size.
std::string wav_header(unsigned channels, unsigned bits_per_sample, std::uint32_t mask,
                       std::uint64_t data_bytes);

}  // namespace joc_mpv
