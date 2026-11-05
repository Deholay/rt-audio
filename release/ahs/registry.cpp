#include <rt_audio/ahs.hpp>
#include <rt_audio/project2/worker.hpp>

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace rt_audio::ahs {

namespace v7_c0291dd1e {
std::unique_ptr<RealtimeProcessor> make(float output_gain);
}
namespace v7_lowlat_c0291dd1e {
std::unique_ptr<RealtimeProcessor> make(float output_gain);
}
namespace v7_lowlat_fir_c0291dd1e {
std::unique_ptr<RealtimeProcessor> make(float output_gain);
}
namespace v7_split_c0291dd1e {
std::unique_ptr<Session> make_session(const StreamConfig& config, float output_gain);
}

namespace {

using ProcessorFactory = std::unique_ptr<RealtimeProcessor> (*)(float);
using SessionFactory = std::unique_ptr<Session> (*)(const StreamConfig&, float);

// A "callback" release inside the default Project2 with an idle analysis worker.
class CallbackSession final : public Session {
public:
    CallbackSession(std::unique_ptr<RealtimeProcessor> processor, const StreamConfig& config)
        : processor_(std::move(processor)), engine_(config, *processor_),
          worker_(engine_, handler_) {}
    AudioCallback& callback() noexcept override { return engine_; }
    void start() override { worker_.start(); }
    void stop() noexcept override { worker_.stop(); }
    void rethrow_if_failed() const override { worker_.rethrow_if_failed(); }

private:
    std::unique_ptr<RealtimeProcessor> processor_;
    Project2 engine_;
    NoopAnalysisHandler handler_;
    AnalysisWorker worker_;
};

struct Factories {
    ProcessorFactory processor; // null for "split" releases
    SessionFactory session;     // null: build a CallbackSession from processor
};

constexpr std::string_view v7_configuration =
    "AFC + notch bank + guard, 16 kHz, block 64, shift off, growth PAPR 15 dB; "
    "Speex Q4 48<->16 kHz (AHS apps/live_pipeline.h)";

// Append new releases at the end; keep both tables in the same order.
constexpr std::string_view v7_lowlat_configuration =
    "AFC + notch bank + guard, 16 kHz, detection every 64 samples, shift off, growth PAPR "
    "15 dB; Speex Q4 48<->16 kHz; no 64-sample pump wait (engine runs on available samples)";

constexpr std::string_view v7_lowlat_fir_configuration =
    "AFC + notch bank + guard, 16 kHz, detection every 64 samples, shift off, growth PAPR "
    "15 dB; linear-phase FIR 48<->16 kHz (111/97 taps, 1.15+1.00 ms, AHS R2 criteria) "
    "instead of Speex Q4; no 64-sample pump wait";

constexpr std::array<Release, 4> table{{
    {"v7-c0291dd1e", "c0291dd1e2051d7934563bbc31daa36bbc044fb4",
     "codex/item2-round45-20261013", v7_configuration, "callback"},
    {"v7-split-c0291dd1e", "c0291dd1e2051d7934563bbc31daa36bbc044fb4",
     "codex/item2-round45-20261013", v7_configuration, "split"},
    {"v7-lowlat-c0291dd1e", "c0291dd1e2051d7934563bbc31daa36bbc044fb4",
     "codex/item2-round45-20261013", v7_lowlat_configuration, "callback"},
    {"v7-lowlat-fir-c0291dd1e", "c0291dd1e2051d7934563bbc31daa36bbc044fb4",
     "codex/item2-round45-20261013", v7_lowlat_fir_configuration, "callback"},
}};
constexpr std::array<Factories, table.size()> factories{{
    {&v7_c0291dd1e::make, nullptr},
    {nullptr, &v7_split_c0291dd1e::make_session},
    {&v7_lowlat_c0291dd1e::make, nullptr},
    {&v7_lowlat_fir_c0291dd1e::make, nullptr},
}};

// "latest" is the newest release, or the newest callback release when
// callback_only (make_processor cannot return a split release).
std::size_t find(std::string_view name, float output_gain, bool callback_only) {
    if (!std::isfinite(output_gain) || output_gain < 0.0F || output_gain > 1.0F) {
        throw std::invalid_argument("AHS output gain must be in [0, 1]");
    }
    if (name == "latest") {
        for (std::size_t i = table.size(); i-- > 0;) {
            if (!callback_only || factories[i].processor) return i;
        }
    }
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].name == name) return i;
    }
    throw std::invalid_argument("Unknown AHS release: " + std::string(name));
}

} // namespace

std::span<const Release> releases() noexcept { return table; }

std::unique_ptr<Session> make_session(std::string_view name, const StreamConfig& config,
                                      float output_gain) {
    const auto& f = factories[find(name, output_gain, false)];
    if (f.session) return f.session(config, output_gain);
    return std::make_unique<CallbackSession>(f.processor(output_gain), config);
}

std::unique_ptr<RealtimeProcessor> make_processor(std::string_view name, float output_gain) {
    const auto i = find(name, output_gain, true);
    if (!factories[i].processor) {
        throw std::invalid_argument("AHS release " + std::string(table[i].name) +
                                    " is a split release; use make_session()");
    }
    return factories[i].processor(output_gain);
}

} // namespace rt_audio::ahs
