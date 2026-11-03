#include <rt_audio/ahs.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>

// Give this release's vendored namespaces unique names so several releases can
// link into one binary. The upstream files themselves stay byte-identical.
#define ahs rt_audio_ahs_v7_c0291dd1e
#define ahs_live rt_audio_ahs_live_v7_c0291dd1e
#include "live_pipeline.h"
#undef ahs_live
#undef ahs

namespace rt_audio::ahs::v7_c0291dd1e {
namespace {

using Pipeline = rt_audio_ahs_live_v7_c0291dd1e::LivePipeline;

// Drives AHS LivePipeline in the same call order as AHS apps/ahs_live_win.cpp
// transfer(): one silent primed period, then capture -> render -> committed.
class Processor final : public RealtimeProcessor {
public:
    explicit Processor(float output_gain) : gain_(output_gain) {}

    void prepare(const StreamConfig& config) override {
        if (config.sample_rate != 48000.0) {
            throw std::invalid_argument("AHS release v7-c0291dd1e requires 48000 Hz");
        }
        // AHS sizes queues as 8 device periods; size for the largest callback.
        pipeline_ = std::make_unique<Pipeline>(std::size_t{max_frames} * 8);
        if (!pipeline_->committed(nullptr, config.frames_per_buffer, config.channels, true)) {
            throw std::runtime_error("AHS reference priming failed");
        }
        failed_ = false;
    }

    void apply_parameters(const ParameterSnapshot&) noexcept override {}

    void process(const ProcessBlock& block) noexcept override {
        // capture() reads all input before render() writes, so in-place is safe.
        failed_ = failed_ ||
                  !pipeline_->capture(block.input, block.frames, block.channels, false);
        if (!failed_) {
            pipeline_->render(block.output, block.frames, block.channels, gain_);
            failed_ = !pipeline_->committed(block.output, block.frames, block.channels, false);
        }
        // AHS reopens the epoch on a broken timeline; the callback cannot
        // allocate, so it stays silent instead. Balanced duplex callbacks never
        // reach this.
        if (failed_) {
            std::fill_n(block.output, std::size_t{block.frames} * block.channels, 0.0F);
        }
    }

private:
    float gain_;
    std::unique_ptr<Pipeline> pipeline_;
    bool failed_ = false;
};

} // namespace

std::unique_ptr<RealtimeProcessor> make(float output_gain) {
    return std::make_unique<Processor>(output_gain);
}

} // namespace rt_audio::ahs::v7_c0291dd1e
