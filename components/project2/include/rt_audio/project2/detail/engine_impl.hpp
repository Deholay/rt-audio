#pragma once

// Template definitions for Engine<Protocol>. Include only where a protocol is
// instantiated (engine.cpp for DefaultProtocol, a release adapter for others).
#include <rt_audio/project2/engine.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace rt_audio {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "Audio counters must be native lock-free atomics");

template <class Protocol>
bool CallbackPort<Protocol>::push_tap(const typename Protocol::Tap& tap) noexcept {
    if (engine_.taps_.try_push(tap)) return true;
    engine_.analysis_dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

template <class Protocol>
template <class Fill>
bool CallbackPort<Protocol>::write_tap(Fill&& fill) noexcept {
    if (engine_.taps_.try_write(std::forward<Fill>(fill))) return true;
    engine_.analysis_dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

template <class Protocol>
bool CallbackPort<Protocol>::pop_command(typename Protocol::Command& command) noexcept {
    return engine_.commands_.try_pop(command);
}

template <class Protocol>
void CallbackPort<Protocol>::command_applied() noexcept {
    engine_.commands_applied_.fetch_add(1, std::memory_order_relaxed);
}

template <class Protocol>
Engine<Protocol>::Engine(StreamConfig config, Processor& processor)
    : config_(config), processor_(processor) {
    if (!std::isfinite(config.sample_rate) || config.sample_rate < 8000.0 ||
        config.sample_rate > 384000.0 || config.channels == 0 ||
        config.channels > max_channels || config.frames_per_buffer == 0 ||
        config.frames_per_buffer > max_frames) {
        throw std::invalid_argument("Invalid stream format or callback size");
    }
    processor_.prepare(config_);
}

template <class Protocol>
bool Engine<Protocol>::process_callback(const float* input, float* output,
                                        std::uint32_t frames, CallbackInfo info) noexcept {
    if (output == nullptr || frames == 0 || frames > max_frames) {
        invalid_callbacks_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    if ((info.status_flags & InputOverflow) != 0) {
        input_overflows_.fetch_add(1, std::memory_order_relaxed);
    }
    if ((info.status_flags & OutputUnderflow) != 0) {
        output_underflows_.fetch_add(1, std::memory_order_relaxed);
    }
    const bool missing = input == nullptr;
    if (missing) {
        input_missing_.fetch_add(1, std::memory_order_relaxed);
        input = silence_.data();
    }
    CallbackPort<Protocol> port(*this);
    protocol_.callback(port, processor_,
                       ProcessBlock{input, output, frames, config_.channels,
                                    config_.sample_rate, next_sample_, info},
                       missing);
    next_sample_ += frames;
    processed_frames_.fetch_add(frames, std::memory_order_relaxed);
    return true;
}

template <class Protocol>
bool Engine<Protocol>::try_read_analysis(Tap& tap) noexcept {
    return taps_.try_pop(tap);
}

template <class Protocol>
bool Engine<Protocol>::try_submit(const Command& command) noexcept {
    const auto sequence = Protocol::sequence(command);
    if (sequence == 0 || sequence <= last_submitted_sequence_ ||
        !Protocol::valid(command, config_)) {
        commands_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!commands_.try_push(command)) {
        commands_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    last_submitted_sequence_ = sequence;
    return true;
}

template <class Protocol>
Statistics Engine<Protocol>::statistics() const noexcept {
    return Statistics{
        callbacks_.load(std::memory_order_relaxed),
        processed_frames_.load(std::memory_order_relaxed),
        analysis_dropped_.load(std::memory_order_relaxed),
        commands_dropped_.load(std::memory_order_relaxed),
        commands_rejected_.load(std::memory_order_relaxed),
        commands_applied_.load(std::memory_order_relaxed),
        input_missing_.load(std::memory_order_relaxed),
        invalid_callbacks_.load(std::memory_order_relaxed),
        input_overflows_.load(std::memory_order_relaxed),
        output_underflows_.load(std::memory_order_relaxed)};
}

} // namespace rt_audio
