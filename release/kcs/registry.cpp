#include <rt_audio/kcs.hpp>

#include <array>
#include <stdexcept>
#include <string>

namespace rt_audio::kcs {

namespace v23_ce6237c12 {
std::unique_ptr<project3::AudioProcessor> make();
}

namespace {

using Factory = std::unique_ptr<project3::AudioProcessor> (*)();

// Append new releases at the end; keep both tables in the same order.
constexpr std::array<Release, 1> table{{
    {"v23-ce6237c12", "ce6237c124a62c79b28667aced2ee2ff6df6b1b0", "main",
     "TransientAR32 (v23, ORDER 32, 16 kHz, 144-sample FIFO) inside Bridge48k "
     "(61-tap Kaiser FIR 48<->16 kHz, global phase); C++ float64 port",
     492},
}};
constexpr std::array<Factory, table.size()> factories{{&v23_ce6237c12::make}};

std::size_t find(std::string_view name) {
    if (name == "latest") return table.size() - 1;
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].name == name) return i;
    }
    throw std::invalid_argument("Unknown KCS release: " + std::string(name));
}

} // namespace

std::span<const Release> releases() noexcept { return table; }

std::unique_ptr<project3::AudioProcessor> make_processor(std::string_view name) {
    return factories[find(name)]();
}

std::uint32_t minimum_delay_frames(std::uint32_t block_frames, std::uint32_t callback_frames,
                                   std::string_view name) {
    if (block_frames == 0 || block_frames > max_device_frames || callback_frames == 0 ||
        callback_frames > max_device_frames) {
        throw std::invalid_argument("KCS spec requires hop and callback in 1..128 frames");
    }
    const std::uint32_t needed = block_frames + table[find(name)].algorithm_delay_frames;
    if (block_frames % callback_frames == 0) return (needed + callback_frames - 1) / callback_frames * callback_frames;
    return needed + callback_frames - 1;
}

project3::StreamConfig recommended_config(std::string_view name, std::uint32_t block_frames) {
    project3::StreamConfig config;
    config.sample_rate = required_sample_rate;
    config.channels = required_channels;
    config.frames_per_buffer = block_frames;
    config.block_frames = block_frames;
    config.lookahead_frames = table[find(name)].algorithm_delay_frames;
    config.delay_frames = minimum_delay_frames(block_frames, block_frames, name);
    return config;
}

} // namespace rt_audio::kcs
