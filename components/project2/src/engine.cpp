#include <rt_audio/project2/detail/engine_impl.hpp>

#include <algorithm>

namespace rt_audio {

template class Engine<DefaultProtocol>;
template class CallbackPort<DefaultProtocol>;

void DefaultProtocol::callback(CallbackPort<DefaultProtocol>& port, RealtimeProcessor& processor,
                               const ProcessBlock& block, bool input_missing) noexcept {
    // Capture before DSP so in-place processing cannot overwrite the analysis tap.
    // Written straight into the queue slot; only frames * channels samples are
    // copied, the rest of the slot is stale (AnalysisBlock documents this).
    (void)port.write_tap([&block, input_missing](AnalysisBlock& tap) noexcept {
        tap.first_sample = block.first_sample;
        tap.input_time_seconds = block.timing.input_time_seconds;
        tap.frames = block.frames;
        tap.channels = block.channels;
        tap.status_flags = block.timing.status_flags;
        tap.input_missing = input_missing ? 1U : 0U;
        std::copy_n(block.input, static_cast<std::size_t>(block.frames) * block.channels,
                    tap.samples.begin());
    });

    // Fixed work budget, even if the worker concurrently keeps publishing.
    // Full snapshots allow us to apply only the newest one consumed this block.
    ParameterSnapshot latest{};
    bool have_update = false;
    for (std::size_t i = 0; i < max_commands_per_callback; ++i) {
        if (!port.pop_command(latest)) break;
        have_update = true;
    }
    if (have_update) {
        processor.apply_parameters(latest);
        port.command_applied();
    }
    processor.process(block);
}

bool DefaultProtocol::valid(const ParameterSnapshot& p, const StreamConfig& config) noexcept {
    if (p.notch_count > max_notches || p.bypass > 1 ||
        !std::isfinite(p.transition_ms) || p.transition_ms < 0.0F ||
        p.transition_ms > 1000.0F) return false;
    const auto nyquist = config.sample_rate * 0.5;
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

} // namespace rt_audio
