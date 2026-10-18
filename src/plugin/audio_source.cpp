#include "audio_source.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"

namespace joc_mpv {
namespace {
constexpr std::size_t kBlockFrames = 1536;  // one E-AC-3 frame
}

Source::Source() = default;

Source::~Source() { close(); }

bool Source::open(const std::string& path, const joc_decode::Settings& settings,
                  double duration_seconds, std::string* error) {
    close();
    std::lock_guard<std::mutex> guard(mutex_);
    int24_ = settings.force_int24;
    duration_ = duration_seconds;
    if (!(duration_ > 0.0)) {
        if (error != nullptr) *error = "the input duration is unknown";
        return false;
    }
    if (!engine_.start(path, settings, error)) {
        engine_.stop();
        return false;
    }
    channels_ = engine_.channels();
    if (channels_ == 0) {
        if (error != nullptr) *error = "the renderer produced no output channels";
        engine_.stop();
        return false;
    }
    total_samples_ = static_cast<std::uint64_t>(std::llround(duration_ * 48000.0));
    produced_bytes_ = 0;
    carry_.clear();
    skip_bytes_ = 0;
    block_.assign(kBlockFrames * channels_, 0.0f);
    if (int24_) packed_.assign(kBlockFrames * channels_ * 3u, 0);
    open_ = true;
    return true;
}

void Source::close() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (open_) engine_.stop();
    open_ = false;
    channels_ = 0;
    total_samples_ = 0;
    produced_bytes_ = 0;
    carry_.clear();
    skip_bytes_ = 0;
}

std::size_t Source::read(std::uint64_t data_byte_offset, std::uint8_t* out, std::size_t count,
                         std::string* error) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!open_) {
        if (error != nullptr) *error = "the renderer is not running";
        return 0;
    }
    const std::uint64_t total = data_bytes();
    if (data_byte_offset >= total) return 0;
    count = static_cast<std::size_t>(std::min<std::uint64_t>(count, total - data_byte_offset));

    const unsigned frame_bytes = bytes_per_frame();
    // What the renderer has produced but the caller has not taken yet.
    const std::uint64_t deliverable = produced_bytes_ - carry_.size();
    if (data_byte_offset != deliverable) {
        const std::uint64_t sample = data_byte_offset / frame_bytes;
        const std::size_t intra = static_cast<std::size_t>(data_byte_offset % frame_bytes);
        joc_log::line("source: reposition to sample %llu (was byte %llu, byte %llu)",
                      static_cast<unsigned long long>(sample),
                      static_cast<unsigned long long>(deliverable),
                      static_cast<unsigned long long>(data_byte_offset));
        if (!engine_.seek(static_cast<double>(sample) / 48000.0, duration_, error)) return 0;
        produced_bytes_ = sample * frame_bytes;
        carry_.clear();
        skip_bytes_ = intra;
    }

    std::size_t written = 0;
    // Leftovers from an earlier block come first: a block is a whole number of
    // frames and the caller may have asked for a smaller piece.
    if (!carry_.empty()) {
        const std::size_t take = std::min(count, carry_.size());
        std::memcpy(out, carry_.data(), take);
        carry_.erase(carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(take));
        written += take;
    }
    while (written < count) {
        const std::uint64_t remaining_samples = total_samples_ - produced_bytes_ / frame_bytes;
        if (remaining_samples == 0) break;
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(kBlockFrames, remaining_samples));
        const std::size_t got = engine_.read(block_.data(), want, error);
        if (got == 0) break;
        produced_bytes_ += static_cast<std::uint64_t>(got) * frame_bytes;

        std::size_t produced = 0;
        const std::uint8_t* source = nullptr;
        if (int24_) {
            joc_decode::pack_int24(block_.data(), got * channels_, packed_.data());
            produced = got * channels_ * 3u;
            source = packed_.data();
        } else {
            produced = got * channels_ * sizeof(float);
            source = reinterpret_cast<const std::uint8_t*>(block_.data());
        }
        std::size_t skip = 0;
        if (skip_bytes_ != 0) {
            skip = std::min(skip_bytes_, produced);
            skip_bytes_ -= skip;
        }
        const std::size_t available = produced - skip;
        const std::size_t take = std::min(count - written, available);
        std::memcpy(out + written, source + skip, take);
        written += take;
        if (take < available) {
            carry_.assign(source + skip + take, source + produced);
        }
    }
    return written;
}

}  // namespace joc_mpv
