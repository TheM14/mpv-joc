#pragma once

#include <cstdint>
#include <string>

namespace joc_mpv {

// Channel mask for a layout the renderer can produce.  The renderer emits its
// channels in the standard WAVE order for that layout, so a WAVE_FORMAT_EXTENSIBLE
// mask tells mpv exactly which speaker each channel belongs to.  Layouts wider
// than 7.1 return 0 (no mask): they cannot reach a WASAPI endpoint anyway.
std::uint32_t channel_mask(const std::string& layout, unsigned channels);

// 68-byte WAVE_FORMAT_EXTENSIBLE header, or a 104-byte RF64 header when the data
// chunk would not fit a 32-bit size.
std::string wav_header(unsigned channels, unsigned bits_per_sample, std::uint32_t mask,
                       std::uint64_t data_bytes);

}  // namespace joc_mpv
