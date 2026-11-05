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
     492, 640},
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

project3::StreamConfig recommended_config(std::string_view name) {
    const Release& release = table[find(name)];
    project3::StreamConfig config;
    config.sample_rate = required_sample_rate;
    config.channels = required_channels;
    config.frames_per_buffer = max_device_frames;
    config.block_frames = max_device_frames;
    config.lookahead_frames = release.algorithm_delay_frames;
    config.delay_frames = release.recommended_delay_frames;
    return config;
}

} // namespace rt_audio::kcs
