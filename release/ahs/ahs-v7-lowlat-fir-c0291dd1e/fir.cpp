// AHS v7 (c0291dd1e), "callback" architecture: v7-lowlat pump + a shorter
// linear-phase FIR resampler pair in place of Speex Q4 (algorithm change).
// Engine (AFC, detector, notches, guard) and its configuration are upstream,
// unchanged; upstream files stay byte-identical. Filters: fir_coeffs.hpp,
// designed and checked against the AHS resampler R2 criteria by design_fir.py.
#include <rt_audio/ahs.hpp>

#include "fir_coeffs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

// Same renaming as ahs-v7-c0291dd1e: identical upstream, identical symbols.
#define ahs rt_audio_ahs_v7_c0291dd1e
#define ahs_live rt_audio_ahs_live_v7_c0291dd1e
#include "live_pipeline.h"
#undef ahs_live
#undef ahs

namespace rt_audio::ahs::v7_lowlat_fir_c0291dd1e {
namespace {

namespace up = ::rt_audio_ahs_v7_c0291dd1e;
namespace live = ::rt_audio_ahs_live_v7_c0291dd1e;

// Streaming 48 -> 16 kHz: y[m] = sum_k h[k] x[3m - k]. Double-written history
// keeps every dot product contiguous; no allocation after construction.
class Decimator3 {
public:
    // Returns true and sets out when this input completes an output sample.
    bool push(float x, float& out) noexcept {
        pos_ = pos_ == 0 ? N - 1 : pos_ - 1;
        hist_[pos_] = hist_[pos_ + N] = x;
        if (++phase_ < 3) return false;
        phase_ = 0;
        float acc = 0.0F;
        for (std::size_t k = 0; k < N; ++k) acc += down_taps[k] * hist_[pos_ + k];
        out = acc;
        return true;
    }

private:
    static constexpr std::size_t N = down_taps.size();
    std::array<float, 2 * N> hist_{};
    std::size_t pos_ = 0;
    int phase_ = 0;
};

// Streaming 16 -> 48 kHz: y[3m + p] = sum_j h[3j + p] x[m - j] (zero-stuffed, gain 3).
class Interpolator3 {
public:
    void push(float x, std::array<float, 3>& out) noexcept {
        pos_ = pos_ == 0 ? M - 1 : pos_ - 1;
        hist_[pos_] = hist_[pos_ + M] = x;
        for (std::size_t p = 0; p < 3; ++p) {
            float acc = 0.0F;
            for (std::size_t j = 0; 3 * j + p < N; ++j) acc += up_taps[3 * j + p] * hist_[pos_ + j];
            out[p] = acc;
        }
    }

private:
    static constexpr std::size_t N = up_taps.size();
    static constexpr std::size_t M = (N + 2) / 3;
    std::array<float, 2 * M> hist_{};
    std::size_t pos_ = 0;
};

constexpr std::size_t max_chunk = 64;

// v7-lowlat pipeline (engine on whatever mic+reference both hold) with the FIR
// pair. Mono mixing, finite checks, silent priming and call order follow
// upstream LivePipeline / RenderReference.
class Pipeline {
public:
    explicit Pipeline(std::size_t capacity)
        : mic_(capacity), reference_(capacity), output_(capacity * 3),
          engine_(live::LivePipeline::config()) {}

    bool capture(const float* input, std::size_t count, unsigned channels) noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            float l = input[i * channels];
            float r = channels == 2 ? input[i * channels + 1] : l;
            if (!std::isfinite(l)) l = 0;
            if (!std::isfinite(r)) r = 0;
            float y;
            if (mic_down_.push(.5f * l + .5f * r, y) && !mic_.push(y)) return false;
        }
        return pump();
    }

    // RenderReference::committed(): mono of what was played; silent = zeros.
    bool committed(const float* final, std::size_t frames, unsigned channels, bool silent) noexcept {
        if ((channels != 1 && channels != 2) || (!silent && !final)) return false;
        for (std::size_t i = 0; i < frames; ++i) {
            float x = 0;
            if (!silent) {
                x = final[i * channels];
                if (channels == 2) x = .5f * x + .5f * final[i * channels + 1];
                if (!std::isfinite(x)) return false;
            }
            float y;
            if (ref_down_.push(x, y) && !reference_.push(y)) return false;
        }
        return pump();
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
            for (std::size_t i = 0; i < n; ++i) {
                std::array<float, 3> o{};
                up_.push(processed[i], o);
                for (float v : o) if (!output_.push(v)) return false;
            }
        }
    }

    live::MonoQueue mic_, reference_, output_;
    Decimator3 mic_down_, ref_down_;
    Interpolator3 up_;
    up::AhsEngine engine_;
};

class Processor final : public RealtimeProcessor {
public:
    explicit Processor(float output_gain) : gain_(output_gain) {}

    void prepare(const StreamConfig& config) override {
        if (config.sample_rate != 48000.0) {
            throw std::invalid_argument("AHS release v7-lowlat-fir-c0291dd1e requires 48000 Hz");
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

} // namespace rt_audio::ahs::v7_lowlat_fir_c0291dd1e
