// Project 3 AudioProcessor around the KCS v23 48 kHz pipeline.
// rt-audio-only behaviour (diverges from KCS v23 Bridge48k, see SPEC.md §1.1):
// non-finite input -> 0; input gap/overflow -> fresh pipeline; no 11 s quota/finish().

#include "pipeline.hpp"

#include <rt_audio/kcs.hpp>

#include <cmath>
#include <stdexcept>
#include <string>

namespace rt_audio::kcs::v23_ce6237c12 {

namespace {

namespace p3 = rt_audio::project3;

class Processor final : public p3::AudioProcessor {
public:
    void prepare(const p3::StreamConfig& config) override {
        if (config.sample_rate != required_sample_rate) {
            throw std::invalid_argument("KCS v23 requires 48000 Hz");
        }
        if (config.channels != required_channels) throw std::invalid_argument("KCS v23 requires mono input");
        if (config.frames_per_buffer > max_device_frames || config.block_frames > max_device_frames) {
            throw std::invalid_argument("KCS spec requires device block and hop <= 128 frames");
        }
        if (config.lookahead_frames < Pipeline48k::delay_frames) {
            throw std::invalid_argument("KCS v23 needs lookahead_frames >= " +
                                        std::to_string(Pipeline48k::delay_frames));
        }
        hop_ = config.block_frames;
        started_ = false;
        have_pending_ = false;
    }

    void on_audio(const p3::AudioBlock& input, p3::OutputSink& sink) override {
        if (!started_ || input.first_sample != expected_ || (input.status_flags & p3::InputOverflow)) {
            restart(input.first_sample);
        }
        expected_ = input.first_sample + input.frames;
        if (have_pending_) have_pending_ = !sink.try_submit_output(pending_);

        for (std::uint32_t i = 0; i < input.frames; ++i) {
            const float x = input.samples[i];
            const float y = pipeline_.tick(std::isfinite(x) ? x : 0.0F);
            const std::uint64_t n = input.first_sample + i;
            if (n < origin_ + Pipeline48k::delay_frames) continue; // before stream start
            out_.samples[n - Pipeline48k::delay_frames - out_.first_sample] = y;
            if (++filled_ == hop_) publish(sink);
        }
    }

private:
    void restart(std::uint64_t first_sample) {
        pipeline_.reset();
        origin_ = first_sample;
        started_ = true;
        have_pending_ = false; // older than anything after the gap
        out_.first_sample = first_sample;
        out_.frames = hop_;
        out_.channels = 1;
        filled_ = 0;
    }

    void publish(p3::OutputSink& sink) {
        // At most one retry for an older block; skipping is allowed (dry fallback).
        if (have_pending_) {
            (void)sink.try_submit_output(pending_);
            have_pending_ = false;
        }
        if (!sink.try_submit_output(out_)) {
            pending_ = out_;
            have_pending_ = true;
        }
        out_.first_sample += hop_;
        filled_ = 0;
    }

    Pipeline48k pipeline_;
    p3::AudioBlock out_{};
    p3::AudioBlock pending_{};
    std::uint64_t origin_ = 0, expected_ = 0;
    std::uint32_t hop_ = 0, filled_ = 0;
    bool started_ = false, have_pending_ = false;
};

} // namespace

std::unique_ptr<p3::AudioProcessor> make() { return std::make_unique<Processor>(); }

} // namespace rt_audio::kcs::v23_ce6237c12
