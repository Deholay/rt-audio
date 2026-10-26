#pragma once

#include <rt_audio/project2/analysis.hpp>
#include <rt_audio/project2/processor.hpp>
#include <rt_audio/project2/detail/spsc_ring.hpp>

#include <atomic>

namespace rt_audio {

class Project2 final : public ParameterSink {
public:
    // Construct and destroy only while both queue endpoints are stopped.
    // Processor must outlive this engine and must not be shared with a worker.
    Project2(StreamConfig config, RealtimeProcessor& processor);
    Project2(const Project2&) = delete;
    Project2& operator=(const Project2&) = delete;

    // Sole audio callback endpoint; valid range 1..max_frames, nonnull output.
    // A null input produces silence as DSP input. Invalid calls return false
    // without touching buffers; the device adapter must silence/abort safely.
    [[nodiscard]] bool process_callback(const float* input, float* output,
                                        std::uint32_t frames,
                                        CallbackInfo info = {}) noexcept;

    // Sole analysis consumer. Do not also call this when AnalysisWorker runs.
    [[nodiscard]] bool try_read_analysis(AnalysisBlock& block) noexcept;
    // Sole parameter producer; UI/control writers must route through this owner.
    [[nodiscard]] bool try_submit_parameters(
        const ParameterSnapshot& parameters) noexcept override;

    [[nodiscard]] const StreamConfig& config() const noexcept { return config_; }
    // Independently sampled counters; not an atomic multi-counter snapshot.
    [[nodiscard]] Statistics statistics() const noexcept;

private:
    bool valid_parameters(const ParameterSnapshot& parameters) const noexcept;
    StreamConfig config_;
    RealtimeProcessor& processor_;
    detail::SpscRing<AnalysisBlock, analysis_capacity> analysis_;
    detail::SpscRing<ParameterSnapshot, command_capacity> commands_;
    AnalysisBlock capture_{};
    std::array<float, max_frames * max_channels> silence_{};
    std::uint64_t next_sample_ = 0; // audio thread only
    std::uint64_t last_submitted_revision_ = 0; // parameter producer only
    std::atomic<std::uint64_t> callbacks_{0}, processed_frames_{0},
        analysis_dropped_{0}, commands_dropped_{0}, commands_rejected_{0},
        commands_applied_{0}, input_missing_{0}, invalid_callbacks_{0},
        input_overflows_{0}, output_underflows_{0};
};

} // namespace rt_audio
