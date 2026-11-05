// KCS release on Project 3: hardware/stream spec enforcement, fixed-delay
// alignment against the bare pipeline, input-gap restart, and a paced
// real-thread run (informational timing only; not an isolated measurement).

#include "../kcs-v23-ce6237c12/pipeline.hpp"

#include <rt_audio/kcs.hpp>
#include <rt_audio/project3.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace p3 = rt_audio::project3;
using rt_audio::kcs::v23_ce6237c12::Pipeline48k;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

void expect_invalid(const std::function<void()>& f, const std::string& what) {
    try {
        f();
        check(false, what + " was accepted");
    } catch (const std::invalid_argument&) {
    }
}

// Synthetic: 3-harmonic tone with a short decaying click every 0.4 s.
std::vector<float> synthetic(std::size_t frames) {
    std::vector<float> x(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        double v = 0.0;
        for (int h = 1; h <= 3; ++h) v += 0.2 / h * std::sin(2.0 * std::numbers::pi * 233.0 * h * t);
        const std::size_t phase = n % 19200;
        if (n > 48000 && phase < 240) v += 0.6 * std::exp(-static_cast<double>(phase) / 40.0) * ((phase % 7) < 3 ? 1 : -1);
        x[n] = static_cast<float>(v);
    }
    return x;
}

std::vector<float> reference(const std::vector<float>& x) {
    Pipeline48k pipeline;
    std::vector<float> y(x.size());
    for (std::size_t n = 0; n < x.size(); ++n) y[n] = pipeline.tick(x[n]);
    check(pipeline.core().counters().triggers > 0, "synthetic input triggers repairs");
    return y;
}

void test_spec() {
    const auto ok = rt_audio::kcs::recommended_config();
    check(ok.sample_rate == 48000.0 && ok.channels == 1 && ok.frames_per_buffer == 128 &&
              ok.block_frames == 128 && ok.lookahead_frames == 492 && ok.delay_frames == 640,
          "recommended config matches SPEC §0");
    rt_audio::kcs::make_processor()->prepare(ok);
    auto bad = [&](auto mutate, const std::string& what) {
        auto config = ok;
        mutate(config);
        expect_invalid([&] { rt_audio::kcs::make_processor()->prepare(config); }, what);
    };
    bad([](p3::StreamConfig& c) { c.sample_rate = 44100.0; }, "44.1 kHz");
    bad([](p3::StreamConfig& c) { c.sample_rate = 16000.0; }, "16 kHz");
    bad([](p3::StreamConfig& c) { c.channels = 2; }, "stereo");
    bad([](p3::StreamConfig& c) { c.frames_per_buffer = 256; }, "256-frame device block");
    bad([](p3::StreamConfig& c) { c.block_frames = 256; c.delay_frames = 768; }, "256-frame hop");
    bad([](p3::StreamConfig& c) { c.lookahead_frames = 256; }, "lookahead below algorithm delay");
    expect_invalid([] { (void)rt_audio::kcs::make_processor("v22-unknown"); }, "unknown release");
    check(rt_audio::kcs::releases().back().name == "v23-ce6237c12", "latest release name");
}

// Synchronous worker after every callback: all post-startup blocks processed,
// output equals the bare pipeline at the fixed delay, sample by sample.
void test_alignment(std::uint32_t callback_frames) {
    auto config = rt_audio::kcs::recommended_config();
    config.frames_per_buffer = callback_frames;
    auto engine = std::make_unique<p3::Engine>(config);
    auto processor = rt_audio::kcs::make_processor();
    processor->prepare(config);
    const auto x = synthetic(48000 * 3);
    const auto y = reference(x);
    std::vector<float> out(callback_frames);
    p3::AudioBlock block{};
    std::size_t mismatches = 0;
    for (std::size_t start = 0; start + callback_frames <= x.size(); start += callback_frames) {
        check(engine->process_callback(x.data() + start, out.data(), callback_frames), "callback accepted");
        for (std::size_t i = 0; i < callback_frames; ++i) {
            const std::size_t t = start + i;
            const float expected = t < config.delay_frames ? 0.0F : y[t - config.delay_frames + Pipeline48k::delay_frames];
            if (out[i] != expected) ++mismatches;
        }
        while (engine->try_read_input(block)) processor->on_audio(block, *engine);
    }
    const auto s = engine->statistics();
    check(mismatches == 0, "fixed-delay output equals pipeline (frames " + std::to_string(callback_frames) +
                               "), mismatches=" + std::to_string(mismatches));
    check(s.fallback_frames == 0 && s.output_rejected == 0 && s.output_stale == 0,
          "no fallback with synchronous worker (frames " + std::to_string(callback_frames) + ")");
}

struct RecordingSink final : p3::OutputSink {
    std::vector<p3::AudioBlock> blocks;
    bool try_submit_output(const p3::AudioBlock& block) noexcept override {
        blocks.push_back(block);
        return true;
    }
    std::uint64_t playback_deadline() const noexcept override { return 0; }
};

// A first_sample jump restarts a fresh pipeline at the new position.
void test_gap() {
    const auto config = rt_audio::kcs::recommended_config();
    auto processor = rt_audio::kcs::make_processor();
    processor->prepare(config);
    const auto x = synthetic(48000 * 2);
    RecordingSink sink;
    const std::uint64_t gap_block = 100, resume_block = 103;
    for (std::uint64_t b = 0; b * 128 + 128 <= x.size(); ++b) {
        if (b >= gap_block && b < resume_block) continue;
        p3::AudioBlock in{};
        in.first_sample = b * 128;
        in.frames = 128;
        in.channels = 1;
        for (std::size_t i = 0; i < 128; ++i) in.samples[i] = x[b * 128 + i];
        processor->on_audio(in, sink);
    }
    const std::vector<float> resumed(x.begin() + resume_block * 128, x.end());
    const auto y = reference(resumed);
    bool ordered = true, content = true, crossed = false;
    std::uint64_t last = 0;
    for (std::size_t k = 0; k < sink.blocks.size(); ++k) {
        const auto& out = sink.blocks[k];
        if (k && out.first_sample <= last) ordered = false;
        last = out.first_sample;
        if (out.first_sample + 128 > gap_block * 128 && out.first_sample < resume_block * 128) crossed = true;
        if (out.first_sample >= resume_block * 128) {
            for (std::size_t i = 0; i < 128; ++i) {
                const std::size_t local = out.first_sample - resume_block * 128 + i + Pipeline48k::delay_frames;
                if (out.samples[i] != y[local]) content = false;
            }
        }
    }
    check(ordered, "gap: published blocks strictly increasing");
    check(!crossed, "gap: no block spanning the missing input");
    check(content, "gap: output after resume equals a fresh pipeline");
}

// Real ProcessingWorker, callbacks paced at 128/48000 s. Timing is machine-
// and load-dependent: reported, not asserted beyond "some blocks processed".
void paced_run() {
    const auto config = rt_audio::kcs::recommended_config();
    auto engine = std::make_unique<p3::Engine>(config);
    auto processor = rt_audio::kcs::make_processor();
    p3::ProcessingWorker worker(*engine, *processor);
    const auto x = synthetic(48000 * 3);
    std::vector<float> out(128);
    worker.start();
    auto next = std::chrono::steady_clock::now();
    for (std::size_t start = 0; start + 128 <= x.size(); start += 128) {
        (void)engine->process_callback(x.data() + start, out.data(), 128);
        next += std::chrono::nanoseconds(2666667);
        std::this_thread::sleep_until(next);
    }
    worker.stop();
    worker.rethrow_if_failed();
    const auto s = engine->statistics();
    check(s.processed_blocks > 0, "paced run processed blocks");
    std::cout << "paced (reference only, not isolated): processed_blocks=" << s.processed_blocks
              << " fallback_blocks=" << s.fallback_blocks << " output_stale=" << s.output_stale
              << " output_late=" << s.output_late << " input_dropped=" << s.input_dropped << '\n';
}

} // namespace

int main() {
    try {
        test_spec();
        test_alignment(128);
        test_alignment(64);
        test_alignment(32);
        test_gap();
        paced_run();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: exception " << e.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "PASS: KCS v23 release on Project 3 (spec, alignment 128/64/32, gap restart)\n";
    return 0;
}
