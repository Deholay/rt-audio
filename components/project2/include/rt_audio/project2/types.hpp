#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rt_audio {

inline constexpr std::uint32_t max_frames = 2048;
inline constexpr std::uint32_t max_channels = 2;
inline constexpr std::size_t max_notches = 16;
inline constexpr std::size_t analysis_capacity = 32;
inline constexpr std::size_t command_capacity = 32;
inline constexpr std::size_t max_commands_per_callback = 4;

struct StreamConfig {
    double sample_rate = 48000.0;
    std::uint32_t channels = 1;
    std::uint32_t frames_per_buffer = 128; // nominal; actual callbacks may vary
};

// Flags intentionally match PortAudio. Other backends must translate theirs.
enum CallbackStatus : std::uint32_t {
    InputUnderflow = 1,
    InputOverflow = 2,
    OutputUnderflow = 4,
    OutputOverflow = 8,
    PrimingOutput = 16,
};

struct CallbackInfo {
    double input_time_seconds = 0.0;
    double output_time_seconds = 0.0;
    std::uint32_t status_flags = 0;
};

// Interleaved float32; frame = one sample per channel. Only frames * channels
// entries are valid. The queue owns a COPY, never the driver's buffer pointer.
struct AnalysisBlock {
    // Delivered frames per channel, not float offset or hardware sample clock.
    // Hardware discontinuities additionally require status_flags/timestamps.
    std::uint64_t first_sample = 0;
    double input_time_seconds = 0.0;
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::uint32_t status_flags = 0;
    std::uint32_t input_missing = 0;
    std::array<float, max_frames * max_channels> samples{};
};

struct NotchTarget {
    float frequency_hz = 1000.0F;
    float bandwidth_hz = 100.0F;
    float depth_db = 0.0F; // nonnegative attenuation, 0 = no attenuation
};

// One complete configuration, not a delta. A callback may coalesce snapshots.
// The algorithm plugin owns coefficient calculation and parameter smoothing.
struct ParameterSnapshot {
    std::uint64_t revision = 0; // strictly increasing, nonzero, per engine lifetime
    std::uint32_t notch_count = 0;
    std::uint32_t bypass = 0; // 0 or 1
    float transition_ms = 5.0F;
    std::array<NotchTarget, max_notches> notches{};
};

struct ProcessBlock {
    const float* input;
    float* output;
    std::uint32_t frames;
    std::uint32_t channels;
    double sample_rate;
    std::uint64_t first_sample;
    CallbackInfo timing;
};

struct Statistics {
    std::uint64_t callbacks;
    std::uint64_t processed_frames;
    std::uint64_t analysis_dropped;
    std::uint64_t commands_dropped;
    std::uint64_t commands_rejected;
    std::uint64_t commands_applied;
    std::uint64_t input_missing;
    std::uint64_t invalid_callbacks;
    std::uint64_t input_overflows;
    std::uint64_t output_underflows;
};

} // namespace rt_audio
