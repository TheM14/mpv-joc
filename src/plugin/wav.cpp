#include "wav.h"

#include <cstring>

namespace joc_mpv {
namespace {

constexpr std::uint32_t kFl = 0x00000001u;
constexpr std::uint32_t kFr = 0x00000002u;
constexpr std::uint32_t kFc = 0x00000004u;
constexpr std::uint32_t kLfe = 0x00000008u;
constexpr std::uint32_t kBl = 0x00000010u;
constexpr std::uint32_t kBr = 0x00000020u;
constexpr std::uint32_t kSl = 0x00000200u;
constexpr std::uint32_t kSr = 0x00000400u;
constexpr std::uint32_t kTfl = 0x00001000u;
constexpr std::uint32_t kTfr = 0x00004000u;
constexpr std::uint32_t kTbl = 0x00008000u;
constexpr std::uint32_t kTbr = 0x00020000u;

void put_u16(std::string* out, std::uint16_t value) {
    char bytes[2];
    std::memcpy(bytes, &value, 2);
    out->append(bytes, 2);
}

void put_u32(std::string* out, std::uint32_t value) {
    char bytes[4];
    std::memcpy(bytes, &value, 4);
    out->append(bytes, 4);
}

void put_u64(std::string* out, std::uint64_t value) {
    char bytes[8];
    std::memcpy(bytes, &value, 8);
    out->append(bytes, 8);
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / PCM, as written by every WAV writer.
void put_subformat(std::string* out, bool is_float) {
    put_u32(out, is_float ? 3u : 1u);
    put_u16(out, 0x0000);
    put_u16(out, 0x0010);
    const unsigned char tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    out->append(reinterpret_cast<const char*>(tail), 8);
}

void put_fmt(std::string* out, unsigned channels, unsigned bits, std::uint32_t mask) {
    const std::uint32_t rate = 48000;
    const std::uint32_t block_align = channels * (bits / 8u);
    out->append("fmt ", 4);
    put_u32(out, 40);
    put_u16(out, 0xFFFE);  // WAVE_FORMAT_EXTENSIBLE
    put_u16(out, static_cast<std::uint16_t>(channels));
    put_u32(out, rate);
    put_u32(out, rate * block_align);
    put_u16(out, static_cast<std::uint16_t>(block_align));
    put_u16(out, static_cast<std::uint16_t>(bits));
    put_u16(out, 22);  // cbSize
    put_u16(out, static_cast<std::uint16_t>(bits));
    put_u32(out, mask);
    put_subformat(out, bits == 32);
}

}  // namespace

std::uint32_t channel_mask(const std::string& layout, unsigned channels) {
    if (layout == "2.0") return kFl | kFr;
    if (layout == "3.1") return kFl | kFr | kFc | kLfe;
    if (layout == "5.1") return kFl | kFr | kFc | kLfe | kBl | kBr;
    if (layout == "7.1") return kFl | kFr | kFc | kLfe | kBl | kBr | kSl | kSr;
    if (layout == "5.1.2") return kFl | kFr | kFc | kLfe | kBl | kBr | kTfl | kTfr;
    if (layout == "5.1.4") {
        return kFl | kFr | kFc | kLfe | kBl | kBr | kTfl | kTfr | kTbl | kTbr;
    }
    if (layout == "7.1.2") {
        return kFl | kFr | kFc | kLfe | kBl | kBr | kSl | kSr | kTfl | kTfr;
    }
    if (layout == "7.1.4") {
        return kFl | kFr | kFc | kLfe | kBl | kBr | kSl | kSr | kTfl | kTfr | kTbl | kTbr;
    }
    // 2-channel binaural output is plain stereo.
    if (channels == 2) return kFl | kFr;
    return 0;
}

std::string wav_header(unsigned channels, unsigned bits_per_sample, std::uint32_t mask,
                       std::uint64_t data_bytes) {
    std::string out;
    const bool rf64 = (data_bytes + 128u) > 0xFFFFFFFFull;
    if (!rf64) {
        out.append("RIFF", 4);
        put_u32(&out, static_cast<std::uint32_t>(data_bytes + 60u));
        out.append("WAVE", 4);
        put_fmt(&out, channels, bits_per_sample, mask);
        out.append("data", 4);
        put_u32(&out, static_cast<std::uint32_t>(data_bytes));
        return out;
    }
    // RF64: the 64-bit sizes live in ds64 and the 32-bit fields are all ones.
    out.append("RF64", 4);
    put_u32(&out, 0xFFFFFFFFu);
    out.append("WAVE", 4);
    out.append("ds64", 4);
    put_u32(&out, 28);
    put_u64(&out, data_bytes + 96u);  // file size - 8
    put_u64(&out, data_bytes);
    put_u64(&out, channels == 0 ? 0 : data_bytes / (channels * (bits_per_sample / 8u)));
    put_u32(&out, 0);  // no table entries
    put_fmt(&out, channels, bits_per_sample, mask);
    out.append("data", 4);
    put_u32(&out, 0xFFFFFFFFu);
    return out;
}

}  // namespace joc_mpv
