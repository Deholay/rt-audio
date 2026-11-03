// AHS v7 (c0291dd1e) split across rt-audio's callback / SPSC / worker architecture.
// Callback: AFC -> notch bank -> guard (sample path). Worker: HowlingDetector.
// The queue payloads are AHS's own types: 64-sample AFC-output hops in, and
// HowlingCandidate lists out. Upstream files are shared with ahs-v7-c0291dd1e.
#include <rt_audio/ahs.hpp>
#include <rt_audio/project2/detail/engine_impl.hpp>
#include <rt_audio/project2/detail/worker_impl.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

// Same renaming as ahs-v7-c0291dd1e: identical upstream, identical symbols.
#define ahs rt_audio_ahs_v7_c0291dd1e
#define ahs_live rt_audio_ahs_live_v7_c0291dd1e
#include "live_pipeline.h"
#undef ahs_live
#undef ahs

namespace rt_audio::ahs::v7_split_c0291dd1e {
namespace {

namespace up = ::rt_audio_ahs_v7_c0291dd1e;
namespace live = ::rt_audio_ahs_live_v7_c0291dd1e;

constexpr int hop_size = 64; // AHS EngineConfig::block_size: one detection per hop
constexpr std::size_t max_candidates = 8; // AHS DetectorConfig::max_candidates

// Callback -> worker: one hop of the detector's input (AFC output, 16 kHz).
struct Hop {
    std::uint64_t sequence = 0; // 1, 2, 3, ... per processed hop
    std::array<float, hop_size> samples{};
};

// Worker -> callback: AHS detector verdict for one hop.
struct Detection {
    std::uint64_t sequence = 0; // sequence of the hop it analysed
    std::uint32_t count = 0;
    std::array<up::HowlingCandidate, max_candidates> candidates{};
};

class Processor;
class Handler;

struct Protocol {
    using Tap = Hop;
    using Command = Detection;
    using Processor = v7_split_c0291dd1e::Processor;
    using Handler = v7_split_c0291dd1e::Handler;
    using Sink = CommandSink<Detection>;
    static constexpr std::size_t tap_capacity = 64;     // 256 ms of hops
    static constexpr std::size_t command_capacity = 64;

    static bool valid(const Detection& d, const StreamConfig&) noexcept {
        return d.count <= d.candidates.size();
    }
    static std::uint64_t sequence(const Detection& d) noexcept { return d.sequence; }
    static void deliver(Handler& handler, const Hop& hop, Sink& sink);
    void callback(CallbackPort<Protocol>& port, Processor& processor, const ProcessBlock& block,
                  bool input_missing) noexcept;
};

// The delivered engine configuration, taken from upstream LivePipeline::config().
up::EngineConfig engine_config() { return live::LivePipeline::config(); }

// AhsEngine::processBlock() and runDetection() from upstream engine.h, with the
// detector moved to the worker. Detections are applied in order as they arrive,
// each advancing AHS's clocks by one hop. The frequency shifter is omitted: it
// is disabled in the delivered configuration (enable_shift = false).
class SplitEngine {
public:
    explicit SplitEngine(const up::EngineConfig& cfg)
        : cfg_(cfg), afc_(cfg.afc, cfg.sample_rate), notches_(cfg.notch, cfg.sample_rate) {
        if (cfg_.block_size != hop_size || cfg_.enable_shift) {
            throw std::invalid_argument("split release expects block 64, shift off");
        }
        freqs_.reserve(max_candidates);
        growth_.reserve(max_candidates);
        const float per_sample_db = cfg_.guard_attack_db_per_ms * 1000.0f / cfg_.sample_rate;
        attack_factor_ = std::pow(10.0f, -per_sample_db / 20.0f);
        release_factor_ = std::pow(10.0f, cfg_.guard_release_db_per_s / (cfg_.sample_rate * 20.0f));
        min_guard_gain_ = std::pow(10.0f, -cfg_.guard_max_reduction_db / 20.0f);
    }

    // One hop: mic/reference in, processed out, AFC output into `tap`.
    void process_hop(const float* mic, const float* loudspeaker, float* out, Hop& tap) noexcept {
        for (int i = 0; i < hop_size; ++i) {
            float s = std::isfinite(mic[i]) ? mic[i] : 0.0f;
            const float x = std::isfinite(loudspeaker[i]) ? loudspeaker[i] : 0.0f;
            if (cfg_.enable_afc) s = afc_.process(s, x);
            tap.samples[static_cast<std::size_t>(i)] = s;
            if (cfg_.enable_notch) s = notches_.process(s);
            const float as = std::abs(s);
            fast_env_ = 0.99f * fast_env_ + 0.01f * as;
            slow_env_ = 0.9995f * slow_env_ + 0.0005f * as;
            if (cfg_.enable_guard) s = apply_guard(s);
            out[i] = s;
        }
        tap.sequence = ++hops_;
    }

    // runDetection() minus the analysis itself and the shifter gate.
    void apply(const Detection& d) noexcept {
        const float elapsed = static_cast<float>(hop_size) / cfg_.sample_rate;
        freqs_.clear();
        growth_.clear();
        for (std::uint32_t i = 0; i < d.count; ++i) {
            freqs_.push_back(d.candidates[i].freq_hz);
            growth_.push_back(d.candidates[i].growth_db_per_s);
        }
        notches_.update(freqs_, growth_, elapsed);
        howling_now_ = !freqs_.empty();
        afc_.setFastAdapt(howling_now_);
        const bool evidence = howling_now_ || notches_.activeCount() > 0;
        engage_hold_s_ = evidence ? cfg_.engage_hold_s
                                  : std::max(0.0f, engage_hold_s_ - elapsed);
        afc_.setSubtractEnabled(engage_hold_s_ > 0.0f);
    }

private:
    float apply_guard(float x) noexcept {
        const bool runaway = fast_env_ > ratio_linear_ * (slow_env_ + 1e-9f);
        if (howling_now_ && runaway) {
            guard_gain_ = std::max(min_guard_gain_, guard_gain_ * attack_factor_);
        } else {
            guard_gain_ = std::min(1.0f, guard_gain_ * release_factor_);
        }
        return x * guard_gain_;
    }

    up::EngineConfig cfg_;
    up::Afc afc_;
    up::NotchBank notches_;
    std::vector<float> freqs_, growth_;
    float guard_gain_ = 1.0f;
    float attack_factor_ = 1.0f, release_factor_ = 1.0f, min_guard_gain_ = 1.0f;
    const float ratio_linear_ = std::pow(10.0f, cfg_.guard_ratio_db / 20.0f);
    bool howling_now_ = false;
    float fast_env_ = 0.0f, slow_env_ = 0.0f;
    float engage_hold_s_ = 0.0f;
    std::uint64_t hops_ = 0;
};

// Upstream LivePipeline with SplitEngine in place of AhsEngine; the 48 <-> 16 kHz
// queues, reference path and call order are the upstream classes unchanged.
class Processor {
public:
    explicit Processor(float output_gain) : gain_(output_gain) {}

    void prepare(const StreamConfig& config) {
        if (config.sample_rate != 48000.0) {
            throw std::invalid_argument("AHS release v7-split-c0291dd1e requires 48000 Hz");
        }
        state_ = std::make_unique<State>(std::size_t{max_frames} * 8);
        if (!state_->reference.committed(nullptr, config.frames_per_buffer, config.channels, true)) {
            throw std::runtime_error("AHS reference priming failed");
        }
        failed_ = false;
    }

    // Port: push_tap(const Hop&) and pop_command(Detection&), command_applied().
    template <class Port>
    void process(const ProcessBlock& block, Port& port) noexcept {
        failed_ = failed_ || !capture(block, port);
        if (!failed_) {
            render(block);
            failed_ = !state_->reference.committed(block.output, block.frames, block.channels,
                                                   false) || !pump(port);
        }
        if (failed_) std::fill_n(block.output, std::size_t{block.frames} * block.channels, 0.0F);
    }

private:
    struct State {
        explicit State(std::size_t capacity)
            : mic(capacity), reference(capacity), output(capacity * 3),
              engine(engine_config()) {}
        live::DownsampleQueue mic;
        live::RenderReference reference;
        live::MonoQueue output;
        SplitEngine engine;
        live::FixedResampler up{16000, 48000};
    };

    // LivePipeline::capture()
    template <class Port>
    bool capture(const ProcessBlock& b, Port& port) noexcept {
        std::array<float, 192> mono{};
        const std::size_t count = b.frames;
        for (std::size_t start = 0; start < count; start += mono.size()) {
            const auto n = std::min(mono.size(), count - start);
            for (std::size_t i = 0; i < n; ++i) {
                float l = b.input[(start + i) * b.channels];
                float r = b.channels == 2 ? b.input[(start + i) * b.channels + 1] : l;
                if (!std::isfinite(l)) l = 0;
                if (!std::isfinite(r)) r = 0;
                mono[i] = .5f * l + .5f * r;
            }
            if (!state_->mic.feed(mono.data(), n) || !pump(port)) return false;
        }
        return true;
    }

    // LivePipeline::render()
    void render(const ProcessBlock& b) noexcept {
        for (std::size_t i = 0; i < b.frames; ++i) {
            float x;
            (void)state_->output.pop(x);
            x = std::isfinite(x) ? std::clamp(x * gain_, -1.0f, 1.0f) : 0;
            for (unsigned ch = 0; ch < b.channels; ++ch) b.output[i * b.channels + ch] = x;
        }
    }

    // LivePipeline::pump(), plus: publish each hop, then apply every detection
    // that has arrived, oldest first.
    template <class Port>
    bool pump(Port& port) noexcept {
        auto& s = *state_;
        while (s.mic.size() >= hop_size && s.reference.size() >= hop_size) {
            std::array<float, hop_size> mic{}, ref{}, processed{};
            for (std::size_t i = 0; i < hop_size; ++i) {
                s.mic.pop(mic[i]);
                s.reference.pop(ref[i]);
            }
            s.engine.process_hop(mic.data(), ref.data(), processed.data(), tap_);
            (void)port.push_tap(tap_);
            Detection d;
            for (std::size_t i = 0; i < Protocol::command_capacity && port.pop_command(d); ++i) {
                s.engine.apply(d);
                port.command_applied();
            }
            std::size_t offset = 0;
            while (offset < hop_size) {
                std::array<float, 192> upsampled{};
                spx_uint32_t ni = static_cast<spx_uint32_t>(hop_size - offset), no = 192;
                if (s.up.process(processed.data() + offset, ni, upsampled.data(), no) !=
                        RESAMPLER_ERR_SUCCESS || (!ni && !no)) return false;
                offset += ni;
                for (spx_uint32_t j = 0; j < no; ++j) {
                    if (!s.output.push(upsampled[j])) return false;
                }
            }
        }
        return true;
    }

    float gain_;
    std::unique_ptr<State> state_;
    Hop tap_{};
    bool failed_ = false;
};

// Worker side: AHS HowlingDetector over a 256-sample window built from hops.
class Handler {
public:
    void prepare(const StreamConfig&) {
        auto cfg = engine_config();
        auto d = cfg.detector;
        d.frame_rate_hz = cfg.sample_rate / static_cast<float>(cfg.block_size);
        detector_ = std::make_unique<up::HowlingDetector>(d, cfg.sample_rate);
        frame_.assign(static_cast<std::size_t>(cfg.detector.fft_size), 0.0f);
    }

    void on_hop(const Hop& hop, CommandSink<Detection>& sink) {
        // A dropped hop (analysis_dropped) leaves a gap; the window simply moves on.
        std::copy(frame_.begin() + hop_size, frame_.end(), frame_.begin());
        std::copy(hop.samples.begin(), hop.samples.end(), frame_.end() - hop_size);
        const auto& candidates = detector_->analyze(frame_.data());
        Detection d;
        d.sequence = hop.sequence;
        d.count = static_cast<std::uint32_t>(std::min(candidates.size(), max_candidates));
        std::copy_n(candidates.begin(), d.count, d.candidates.begin());
        // Every detection matters (NotchBank is stateful): retry briefly, then
        // drop (counted as commands_dropped) so stop() can still join.
        for (int attempt = 0; attempt < 50 && !sink.try_submit(d); ++attempt) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

private:
    std::unique_ptr<up::HowlingDetector> detector_;
    std::vector<float> frame_;
};

void Protocol::deliver(Handler& handler, const Hop& hop, Sink& sink) { handler.on_hop(hop, sink); }

void Protocol::callback(CallbackPort<Protocol>& port, Processor& processor,
                        const ProcessBlock& block, bool) noexcept {
    processor.process(block, port);
}

class SplitSession final : public Session {
public:
    SplitSession(const StreamConfig& config, float output_gain)
        : processor_(output_gain), engine_(config, processor_), worker_(engine_, handler_) {}
    AudioCallback& callback() noexcept override { return engine_; }
    void start() override { worker_.start(); }
    void stop() noexcept override { worker_.stop(); }
    void rethrow_if_failed() const override { worker_.rethrow_if_failed(); }

private:
    Processor processor_;
    Engine<Protocol> engine_;
    Handler handler_;
    Worker<Protocol> worker_;
};

} // namespace

std::unique_ptr<Session> make_session(const StreamConfig& config, float output_gain) {
    return std::make_unique<SplitSession>(config, output_gain);
}

} // namespace rt_audio::ahs::v7_split_c0291dd1e
