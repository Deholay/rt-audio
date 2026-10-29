#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rt_audio::project3 {

inline constexpr std::uint32_t max_frames = 2048;
inline constexpr std::uint32_t max_channels = 2;
inline constexpr std::uint32_t max_delay_frames = 16384;
inline constexpr std::size_t queue_capacity = 32;
inline constexpr std::size_t max_output_pops_per_callback = queue_capacity;

struct StreamConfig {
    double sample_rate = 48000.0;
    std::uint32_t channels = 1;
    std::uint32_t frames_per_buffer = 128; // requested device callback size
    std::uint32_t block_frames = 128; // worker input/output hop size
    std::uint32_t lookahead_frames = 256; // reserved algorithm context budget
    std::uint32_t delay_frames = 512; // exact software delay, including lookahead
};

// Same portable status bits as PortAudio. Other hosts must translate theirs.
enum CallbackStatus : std::uint32_t {
    InputUnderflow = 1, InputOverflow = 2, OutputUnderflow = 4,
    OutputOverflow = 8, PrimingOutput = 16,
};

struct CallbackInfo {
    double input_time_seconds = 0.0;
    double output_time_seconds = 0.0;
    std::uint32_t status_flags = 0;
};

// Input and processed output share this format. Output retains the ORIGINAL
// first_sample, frames and channels; do not shift it by lookahead/delay.
// first_sample counts delivered input frames (per channel), not hardware time.
// Device gaps require status flags/timestamps; queue gaps appear in first_sample.
struct AudioBlock {
    std::uint64_t first_sample = 0;
    double input_time_seconds = 0.0;
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::uint32_t status_flags = 0;
    std::uint32_t input_missing = 0;
    std::array<float, max_frames * max_channels> samples{};
};

struct Statistics {
    std::uint64_t callbacks;
    std::uint64_t captured_frames;
    std::uint64_t rendered_frames;
    std::uint64_t startup_frames;
    std::uint64_t processed_frames;
    std::uint64_t fallback_frames;
    std::uint64_t processed_blocks;
    std::uint64_t fallback_blocks;
    std::uint64_t input_dropped;
    std::uint64_t output_dropped;
    std::uint64_t output_late; // rejected at producer because deadline was committed
    std::uint64_t output_stale; // discarded at consumer, having arrived too late
    std::uint64_t output_rejected; // format, order or uncaptured source range
    std::uint64_t input_missing; // callbacks, not frames
    std::uint64_t invalid_callbacks;
    std::uint64_t input_overflows;
    std::uint64_t output_underflows;
};

} // namespace rt_audio::project3
