#include <rt_audio/project2/engine.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rt_audio {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "Audio counters must be native lock-free atomics");

Project2::Project2(StreamConfig config, RealtimeProcessor& processor)
    : config_(config), processor_(processor) {
    if (!std::isfinite(config.sample_rate) || config.sample_rate < 8000.0 ||
        config.sample_rate > 384000.0 || config.channels == 0 ||
        config.channels > max_channels || config.frames_per_buffer == 0 ||
        config.frames_per_buffer > max_frames) {
        throw std::invalid_argument("Invalid stream format or callback size");
    }
    processor_.prepare(config_);
}

bool Project2::process_callback(const float* input, float* output,
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

    // Capture before DSP so in-place processing cannot overwrite the analysis tap.
    capture_.first_sample = next_sample_;
    capture_.input_time_seconds = info.input_time_seconds;
    capture_.frames = frames;
    capture_.channels = config_.channels;
    capture_.status_flags = info.status_flags;
    capture_.input_missing = missing ? 1U : 0U;
    std::copy_n(input, static_cast<std::size_t>(frames) * config_.channels,
                capture_.samples.begin());
    if (!analysis_.try_push(capture_)) {
        analysis_dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // Fixed work budget, even if the worker concurrently keeps publishing.
    // Full snapshots allow us to apply only the newest one consumed this block.
    ParameterSnapshot latest{};
    bool have_update = false;
    for (std::size_t i = 0; i < max_commands_per_callback; ++i) {
        if (!commands_.try_pop(latest)) break;
        have_update = true;
    }
    if (have_update) {
        processor_.apply_parameters(latest);
        commands_applied_.fetch_add(1, std::memory_order_relaxed);
    }
    processor_.process(ProcessBlock{input, output, frames, config_.channels,
                                    config_.sample_rate, next_sample_, info});
    next_sample_ += frames;
    processed_frames_.fetch_add(frames, std::memory_order_relaxed);
    return true;
}

bool Project2::try_read_analysis(AnalysisBlock& block) noexcept {
    return analysis_.try_pop(block);
}

bool Project2::valid_parameters(const ParameterSnapshot& p) const noexcept {
    if (p.revision == 0 || p.revision <= last_submitted_revision_ ||
        p.notch_count > max_notches || p.bypass > 1 ||
        !std::isfinite(p.transition_ms) || p.transition_ms < 0.0F ||
        p.transition_ms > 1000.0F) return false;
    const auto nyquist = config_.sample_rate * 0.5;
    for (std::uint32_t i = 0; i < p.notch_count; ++i) {
        const auto& n = p.notches[i];
        if (!std::isfinite(n.frequency_hz) || n.frequency_hz <= 0.0F ||
            n.frequency_hz >= nyquist || !std::isfinite(n.bandwidth_hz) ||
            n.bandwidth_hz <= 0.0F || n.bandwidth_hz >= nyquist ||
            !std::isfinite(n.depth_db) || n.depth_db < 0.0F ||
            n.depth_db > 120.0F) return false;
    }
    return true;
}

bool Project2::try_submit_parameters(const ParameterSnapshot& parameters) noexcept {
    if (!valid_parameters(parameters)) {
        commands_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!commands_.try_push(parameters)) {
        commands_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    last_submitted_revision_ = parameters.revision;
    return true;
}

Statistics Project2::statistics() const noexcept {
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
