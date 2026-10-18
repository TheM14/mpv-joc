#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "joc_decode.h"

namespace joc_mpv {

// Rendered audio as a seekable byte stream: the HTTP server asks for a byte
// range of the WAV data chunk, this maps it to a sample index, repositions the
// engine when the request does not continue where the last one stopped, and
// returns the requested bytes.
class Source {
public:
    Source();
    ~Source();
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    bool open(const std::string& path, const joc_decode::Settings& settings, double duration_seconds,
              std::string* error);
    void close();

    unsigned channels() const { return channels_; }
    unsigned bits_per_sample() const { return int24_ ? 24u : 32u; }
    unsigned bytes_per_frame() const { return channels_ * (bits_per_sample() / 8u); }
    std::uint64_t total_samples() const { return total_samples_; }
    std::uint64_t data_bytes() const { return total_samples_ * bytes_per_frame(); }
    double duration_seconds() const { return duration_; }

    // Fills up to count bytes of the data chunk starting at data_byte_offset.
    std::size_t read(std::uint64_t data_byte_offset, std::uint8_t* out, std::size_t count,
                     std::string* error);

private:
    std::mutex mutex_;
    joc_decode::Engine engine_;
    bool open_ = false;
    bool int24_ = false;
    unsigned channels_ = 0;
    double duration_ = 0.0;
    std::uint64_t total_samples_ = 0;
    // Bytes the renderer has produced since the last reposition, counted from
    // the start of the data chunk.  Tracking bytes rather than samples keeps a
    // request that starts inside a frame from forcing a reposition on every
    // following read.
    std::uint64_t produced_bytes_ = 0;
    // Bytes produced by the renderer that the caller did not ask for yet.
    std::vector<std::uint8_t> carry_;
    std::size_t skip_bytes_ = 0;
    std::vector<float> block_;
    std::vector<std::uint8_t> packed_;
};

}  // namespace joc_mpv
