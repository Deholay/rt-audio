// AHS v7 (c0291dd1e), "callback" architecture, low-latency pump.
// Upstream LivePipeline waits for a full 64-sample hop before calling the
// engine; this release runs the engine on whatever 16 kHz samples the mic and
// reference queues both hold. AhsEngine counts samples internally, so its
// detection cadence (every 64 samples) is unchanged; only the 64-sample
// buffering wait (~2.67 ms at 48 kHz) is removed. Upstream files are shared
// with ahs-v7-c0291dd1e and stay byte-identical.
#include <rt_audio/ahs.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>

// Same renaming as ahs-v7-c0291dd1e: identical upstream, identical symbols.
#define ahs rt_audio_ahs_v7_c0291dd1e
#define ahs_live rt_audio_ahs_live_v7_c0291dd1e
#include "live_pipeline.h"
#undef ahs_live
#undef ahs

namespace rt_audio::ahs::v7_lowlat_c0291dd1e {
namespace {

namespace up = ::rt_audio_ahs_v7_c0291dd1e;
namespace live = ::rt_audio_ahs_live_v7_c0291dd1e;

constexpr std::size_t max_chunk = 64; // engine call size cap; any size is valid

// live::LivePipeline with pump() changed from "exactly 64 when 64 are ready"
// to "everything both queues hold, in chunks of up to 64". capture(),
// committed() and render() are the upstream bodies.
class Pipeline {
public:
    explicit Pipeline(std::size_t capacity)
        : mic_(capacity), reference_(capacity), output_(capacity * 3),
          engine_(live::LivePipeline::config()) {}

    bool capture(const float* input, std::size_t count, unsigned channels) noexcept {
        std::array<float, 192> mono{};
        for (std::size_t start = 0; start < count; start += mono.size()) {
            const auto n = std::min(mono.size(), count - start);
            for (std::size_t i = 0; i < n; ++i) {
                float l = input[(start + i) * channels];
                float r = channels == 2 ? input[(start + i) * channels + 1] : l;
                if (!std::isfinite(l)) l = 0;
                if (!std::isfinite(r)) r = 0;
                mono[i] = .5f * l + .5f * r;
            }
            if (!mic_.feed(mono.data(), n) || !pump()) return false;
        }
        return true;
    }

    bool committed(const float* final, std::size_t frames, unsigned channels, bool silent) noexcept {
        return reference_.committed(final, frames, channels, silent) && pump();
    }

    void render(float* final, std::size_t frames, unsigned channels, float gain) noexcept {
        for (std::size_t i = 0; i < frames; ++i) {
            float x;
            (void)output_.pop(x);
            x = std::isfinite(x) ? std::clamp(x * gain, -1.0f, 1.0f) : 0;
            for (unsigned ch = 0; ch < channels; ++ch) final[i * channels + ch] = x;
        }
    }

private:
    bool pump() noexcept {
        while (true) {
            const std::size_t n = std::min({mic_.size(), reference_.size(), max_chunk});
            if (n == 0) return true;
            std::array<float, max_chunk> mic{}, ref{}, processed{};
            for (std::size_t i = 0; i < n; ++i) {
                mic_.pop(mic[i]);
                reference_.pop(ref[i]);
            }
            engine_.processBlock(mic.data(), ref.data(), processed.data(), static_cast<int>(n));
            std::size_t offset = 0;
            while (offset < n) {
                std::array<float, 192> upsampled{};
                spx_uint32_t ni = static_cast<spx_uint32_t>(n - offset), no = 192;
                if (up_.process(processed.data() + offset, ni, upsampled.data(), no) !=
                        RESAMPLER_ERR_SUCCESS || (!ni && !no)) return false;
                offset += ni;
                for (spx_uint32_t j = 0; j < no; ++j) {
                    if (!output_.push(upsampled[j])) return false;
                }
            }
        }
    }

    live::DownsampleQueue mic_;
    live::RenderReference reference_;
    live::MonoQueue output_;
    up::AhsEngine engine_;
    live::FixedResampler up_{16000, 48000};
};

// Same call order as ahs-v7-c0291dd1e (AHS ahs_live_win.cpp transfer()).
class Processor final : public RealtimeProcessor {
public:
    explicit Processor(float output_gain) : gain_(output_gain) {}

    void prepare(const StreamConfig& config) override {
        if (config.sample_rate != 48000.0) {
            throw std::invalid_argument("AHS release v7-lowlat-c0291dd1e requires 48000 Hz");
        }
        pipeline_ = std::make_unique<Pipeline>(std::size_t{max_frames} * 8);
        if (!pipeline_->committed(nullptr, config.frames_per_buffer, config.channels, true)) {
            throw std::runtime_error("AHS reference priming failed");
        }
        failed_ = false;
    }

    void apply_parameters(const ParameterSnapshot&) noexcept override {}

    void process(const ProcessBlock& block) noexcept override {
        failed_ = failed_ || !pipeline_->capture(block.input, block.frames, block.channels);
        if (!failed_) {
            pipeline_->render(block.output, block.frames, block.channels, gain_);
            failed_ = !pipeline_->committed(block.output, block.frames, block.channels, false);
        }
        if (failed_) std::fill_n(block.output, std::size_t{block.frames} * block.channels, 0.0F);
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

} // namespace rt_audio::ahs::v7_lowlat_c0291dd1e
