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

// One ported KCS snapshot under release/kcs/kcs-<name>/.
struct Release {
    std::string_view name;          // e.g. "v23-ce6237c12"
    std::string_view source_commit; // full KCS commit the reference was copied from
    std::string_view source_branch;
    std::string_view configuration; // what the release runs (for humans)
    std::uint32_t algorithm_delay_frames; // 48 kHz; minimum lookahead_frames
    std::uint32_t recommended_delay_frames;
};

// Oldest first; the last entry is the newest release.
[[nodiscard]] std::span<const Release> releases() noexcept;

// "latest" or a releases() name. Unknown name: std::invalid_argument.
// prepare() rejects configs outside the spec above or with
// lookahead_frames < algorithm_delay_frames (std::invalid_argument).
[[nodiscard]] std::unique_ptr<project3::AudioProcessor> make_processor(std::string_view name = "latest");

// 48 kHz, mono, 128-frame callbacks and hops, lookahead = algorithm delay,
// delay = recommended_delay_frames.
[[nodiscard]] project3::StreamConfig recommended_config(std::string_view name = "latest");

} // namespace rt_audio::kcs
