#pragma once

#include <rt_audio/project3/processor.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace rt_audio::kcs {

// Hardware/stream spec (release/kcs/SPEC.md §0). Contract: KCS item3/test_plan_v3 §4.
inline constexpr double required_sample_rate = 48000.0;
inline constexpr std::uint32_t required_channels = 1;
inline constexpr std::uint32_t max_device_frames = 128; // frames_per_buffer and block_frames
// Default hop = callback: lowest software delay (544 frames, 11.33 ms) and smaller
// device buffers than 128; fallback rate in paced runs was similar (notebook kcs_rt §4d).
inline constexpr std::uint32_t recommended_block_frames = 32;

// One ported KCS snapshot under release/kcs/kcs-<name>/.
struct Release {
    std::string_view name;          // e.g. "v23-ce6237c12"
    std::string_view source_commit; // full KCS commit the reference was copied from
    std::string_view source_branch;
    std::string_view configuration; // what the release runs (for humans)
    std::uint32_t algorithm_delay_frames; // 48 kHz; minimum lookahead_frames
};

// Oldest first; the last entry is the newest release.
[[nodiscard]] std::span<const Release> releases() noexcept;

// "latest" or a releases() name. Unknown name: std::invalid_argument.
// prepare() rejects configs outside the spec above or with
// lookahead_frames < algorithm_delay_frames (std::invalid_argument).
[[nodiscard]] std::unique_ptr<project3::AudioProcessor> make_processor(std::string_view name = "latest");

// Smallest fixed delay at which a hop's result can be ready one callback after
// its last needed input (hop + algorithm delay) is captured, assuming fixed-size
// callbacks: ceil((hop + algorithm) / callback) * callback when callback divides
// hop, else the bound hop + algorithm + callback - 1. The worker gets about one
// callback period; a late block plays aligned dry. Hop/callback in 1..128.
[[nodiscard]] std::uint32_t minimum_delay_frames(std::uint32_t block_frames, std::uint32_t callback_frames,
                                                 std::string_view name = "latest");

// 48 kHz, mono, callback = hop = block_frames (1..128), lookahead = algorithm
// delay, delay = minimum_delay_frames(block_frames, block_frames).
// 128 -> 640 (13.33 ms), 64 -> 576 (12.00 ms), 32 -> 544 (11.33 ms).
[[nodiscard]] project3::StreamConfig recommended_config(std::string_view name = "latest",
                                                        std::uint32_t block_frames = recommended_block_frames);

} // namespace rt_audio::kcs
