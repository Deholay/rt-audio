// Offline driver for notebook/kcs_rt.ipynb. Runs the real rt-audio Project 3
// engine/worker with a KCS release; audio I/O is raw float32 files, results are
// one JSON line.
//
//   run <passthrough|kcs-latest|kcs-<release>> <in.f32> <out_prefix> <hop> <sync|paced>
//   latency <hop>
//   stages <in.f32>
//   comm <in.f32> <hop>
// hop = worker block = device callback (1..128); delay = kcs::minimum_delay_frames(hop, hop).
#include "../release/kcs/kcs-v23-ce6237c12/pipeline.hpp"

#include <rt_audio/kcs.hpp>
#include <rt_audio/project3.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace p3 = rt_audio::project3;
using rt_audio::kcs::v23_ce6237c12::Core16k;
using rt_audio::kcs::v23_ce6237c12::Pipeline48k;
using clk = std::chrono::steady_clock;
constexpr auto callback_period = std::chrono::nanoseconds(2666667); // 128 / 48000 s

std::vector<float> load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    const auto bytes = f.tellg();
    std::vector<float> v(static_cast<std::size_t>(bytes) / sizeof(float));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(float)));
    return v;
}

void save(const std::string& path, const std::vector<float>& v) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(float)));
    if (!f) throw std::runtime_error("cannot write " + path);
}

double us(clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::micro>(b - a).count(); }

// {"mean":..,"p50":..,"p99":..,"max":..,"n":..}
std::string summary(std::vector<double> v) {
    if (v.empty()) return "null";
    double sum = 0;
    for (double x : v) sum += x;
    std::sort(v.begin(), v.end());
    char buf[200];
    std::snprintf(buf, sizeof buf, "{\"mean\":%.4f,\"p50\":%.4f,\"p99\":%.4f,\"max\":%.3f,\"n\":%zu}",
                  sum / static_cast<double>(v.size()), v[v.size() / 2], v[v.size() * 99 / 100], v.back(), v.size());
    return buf;
}

std::string stats_json(const p3::Statistics& s) {
    char buf[600];
    std::snprintf(buf, sizeof buf,
                  "{\"callbacks\":%llu,\"startup_frames\":%llu,\"processed_blocks\":%llu,\"fallback_blocks\":%llu,"
                  "\"processed_frames\":%llu,\"fallback_frames\":%llu,\"input_dropped\":%llu,\"output_dropped\":%llu,"
                  "\"output_late\":%llu,\"output_stale\":%llu,\"output_rejected\":%llu}",
                  (unsigned long long)s.callbacks, (unsigned long long)s.startup_frames,
                  (unsigned long long)s.processed_blocks, (unsigned long long)s.fallback_blocks,
                  (unsigned long long)s.processed_frames, (unsigned long long)s.fallback_frames,
                  (unsigned long long)s.input_dropped, (unsigned long long)s.output_dropped,
                  (unsigned long long)s.output_late, (unsigned long long)s.output_stale,
                  (unsigned long long)s.output_rejected);
    return buf;
}

std::unique_ptr<p3::AudioProcessor> make(const std::string& name) {
    if (name == "passthrough") return std::make_unique<p3::PassthroughProcessor>();
    if (name.rfind("kcs-", 0) == 0) return rt_audio::kcs::make_processor(name.substr(4));
    throw std::invalid_argument("unknown processor " + name);
}

// Engine output for the whole input plus delay_frames of zeros (so every input
// sample is played); output[t] plays source t - delay_frames.
int run(char** a) {
    const std::string name = a[2];
    auto x = load(a[3]);
    const std::string prefix = a[4];
    const auto frames = static_cast<std::uint32_t>(std::stoul(a[5]));
    const bool paced = std::string(a[6]) == "paced";
    // Same config for passthrough -> same alignment.
    const auto config = rt_audio::kcs::recommended_config("latest", frames);
    auto engine = std::make_unique<p3::Engine>(config);
    auto processor = make(name);
    p3::ProcessingWorker worker(*engine, *processor);
    const std::size_t total = (x.size() + config.delay_frames + frames - 1) / frames * frames;
    x.resize(total, 0.0F);
    std::vector<float> out(total, 0.0F);
    std::vector<float> cb_us;
    p3::AudioBlock block{};
    if (paced) worker.start();
    else processor->prepare(config);
    auto next = clk::now();
    for (std::size_t s = 0; s < total; s += frames) {
        const auto t0 = clk::now();
        if (!engine->process_callback(x.data() + s, out.data() + s, frames)) throw std::runtime_error("callback rejected");
        cb_us.push_back(static_cast<float>(us(t0, clk::now())));
        if (paced) {
            next += callback_period * frames / 128;
            std::this_thread::sleep_until(next);
        } else {
            while (engine->try_read_input(block)) processor->on_audio(block, *engine);
        }
    }
    if (paced) {
        worker.stop();
        worker.rethrow_if_failed();
    }
    save(prefix + "_out.f32", out);
    save(prefix + "_cb_us.f32", cb_us);
    // Trigger count from the bare pipeline on the same input (processor state is private).
    Pipeline48k ref;
    for (std::size_t n = 0; n + config.delay_frames < total; ++n) (void)ref.tick(x[n]);
    const auto& c = ref.core().counters();
    std::printf("{\"processor\":\"%s\",\"mode\":\"%s\",\"frames\":%u,\"delay_frames\":%u,\"algorithm_delay_frames\":%u,"
                "\"triggers\":%llu,\"bypass\":%llu,\"low_energy\":%llu,\"stats\":%s}\n",
                name.c_str(), paced ? "paced" : "sync", frames, config.delay_frames, Pipeline48k::delay_frames,
                name == "passthrough" ? 0ULL : (unsigned long long)c.triggers,
                name == "passthrough" ? 0ULL : (unsigned long long)c.bypass,
                name == "passthrough" ? 0ULL : (unsigned long long)c.low_energy,
                stats_json(engine->statistics()).c_str());
    return 0;
}

template <typename F>
long impulse_lag(std::size_t n, std::size_t at, F&& tick) {
    long best = -1;
    double peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double y = std::abs(tick(i == at ? 0.5 : 0.0));
        if (y > peak) { peak = y; best = static_cast<long>(i); }
    }
    return best - static_cast<long>(at);
}

// Impulse per module. Core in silence: low_energy bypass, so the impulse passes unchanged.
int latency(char** a) {
    const auto frames = static_cast<std::uint32_t>(std::stoul(a[2]));
    Core16k core;
    const long core_lag = impulse_lag(2000, 900, [&](double v) { return core.tick(v); });
    long pipe_lag[3];
    for (int ph = 0; ph < 3; ++ph) {
        Pipeline48k p;
        pipe_lag[ph] = impulse_lag(6000, 3000 + ph, [&](double v) { return p.tick(static_cast<float>(v)); });
    }
    const auto config = rt_audio::kcs::recommended_config("latest", frames);
    p3::Engine engine(config);
    auto proc = rt_audio::kcs::make_processor();
    proc->prepare(config);
    std::vector<float> in(24000, 0.0F), out(24000, 0.0F);
    const std::size_t at = 12000;
    in[at] = 0.5F;
    p3::AudioBlock b{};
    for (std::size_t s = 0; s + frames <= in.size(); s += frames) {
        (void)engine.process_callback(in.data() + s, out.data() + s, frames);
        while (engine.try_read_input(b)) proc->on_audio(b, engine);
    }
    long best = -1;
    float peak = 0.0F;
    for (std::size_t i = 0; i < out.size(); ++i) if (std::abs(out[i]) > peak) { peak = std::abs(out[i]); best = (long)i; }
    std::printf("{\"frames\":%u,\"core_lag_16k\":%ld,\"pipeline_lag_48k\":[%ld,%ld,%ld],\"engine_lag_48k\":%ld,"
                "\"delay_frames\":%u,\"lookahead_frames\":%u}\n",
                frames, core_lag, pipe_lag[0], pipe_lag[1], pipe_lag[2], best - (long)at, config.delay_frames,
                config.lookahead_frames);
    return 0;
}

// Compute per module, per 128-frame block (2.667 ms of audio). Reference only.
int stages(char** a) {
    const auto x = load(a[2]);
    std::vector<double> pipe_plain, pipe_trig, core_plain, core_trig, cb, worker;
    {
        Pipeline48k p;
        std::uint64_t trig = 0;
        for (std::size_t s = 0; s + 128 <= x.size(); s += 128) {
            const auto t0 = clk::now();
            for (int i = 0; i < 128; ++i) (void)p.tick(x[s + i]);
            const double d = us(t0, clk::now());
            if (p.core().counters().triggers != trig) { trig = p.core().counters().triggers; pipe_trig.push_back(d); }
            else pipe_plain.push_back(d);
        }
    }
    {
        // Same 16 kHz samples the core sees inside the bridge (decimated, unfiltered here).
        Core16k c;
        std::uint64_t trig = 0;
        std::size_t n = 0;
        for (std::size_t s = 0; s + 128 <= x.size(); s += 128) {
            const auto t0 = clk::now();
            for (; n < s + 128; ++n) if (n % 3 == 0) (void)c.tick(x[n]);
            const double d = us(t0, clk::now());
            if (c.counters().triggers != trig) { trig = c.counters().triggers; core_trig.push_back(d); }
            else core_plain.push_back(d);
        }
    }
    {
        const auto config = rt_audio::kcs::recommended_config("latest", 128);
        p3::Engine engine(config);
        auto proc = rt_audio::kcs::make_processor();
        proc->prepare(config);
        std::vector<float> out(128);
        p3::AudioBlock b{};
        for (std::size_t s = 0; s + 128 <= x.size(); s += 128) {
            const auto t0 = clk::now();
            (void)engine.process_callback(x.data() + s, out.data(), 128);
            cb.push_back(us(t0, clk::now()));
            while (engine.try_read_input(b)) {
                const auto t1 = clk::now();
                proc->on_audio(b, engine);
                worker.push_back(us(t1, clk::now()));
            }
        }
    }
    std::printf("{\"unit\":\"us per 128-frame block\",\"pipeline_no_trigger\":%s,\"pipeline_trigger\":%s,"
                "\"core_no_trigger\":%s,\"core_trigger\":%s,\"engine_callback\":%s,\"on_audio\":%s}\n",
                summary(pipe_plain).c_str(), summary(pipe_trig).c_str(), summary(core_plain).c_str(),
                summary(core_trig).c_str(), summary(cb).c_str(), summary(worker).c_str());
    return 0;
}

// Timestamps on the real Engine + ProcessingWorker, callbacks paced at hop/48000 s.
// Input block m: captured by callback m, read by the worker; its output is played
// by callback m+1 (delay = ceil((hop + 492) / hop) * hop).
int comm(char** a) {
    const auto x = load(a[2]);
    const auto hop = static_cast<std::uint32_t>(std::stoul(a[3]));
    const std::size_t blocks = x.size() / hop;
    struct Timed final : p3::AudioProcessor {
        std::unique_ptr<p3::AudioProcessor> inner = rt_audio::kcs::make_processor();
        std::vector<clk::time_point> enter, leave;
        void prepare(const p3::StreamConfig& c) override { inner->prepare(c); }
        void on_audio(const p3::AudioBlock& in, p3::OutputSink& s) override {
            const auto m = in.first_sample / in.frames;
            const auto t0 = clk::now();
            inner->on_audio(in, s);
            if (m < enter.size()) { enter[m] = t0; leave[m] = clk::now(); }
        }
    } timed;
    timed.enter.assign(blocks, {});
    timed.leave.assign(blocks, {});
    std::vector<clk::time_point> cb_start(blocks + 1), cb_end(blocks);
    const auto config = rt_audio::kcs::recommended_config("latest", hop);
    p3::Engine engine(config);
    p3::ProcessingWorker worker(engine, timed);
    std::vector<float> out(hop);
    worker.start();
    auto next = clk::now();
    for (std::size_t m = 0; m < blocks; ++m) {
        cb_start[m] = clk::now();
        (void)engine.process_callback(x.data() + m * hop, out.data(), hop);
        cb_end[m] = clk::now();
        next += callback_period * hop / 128;
        std::this_thread::sleep_until(next);
    }
    cb_start[blocks] = clk::now();
    worker.stop();
    worker.rethrow_if_failed();
    std::vector<double> transit, work, wait, round, over;
    std::size_t late = 0;
    for (std::size_t m = 0; m < blocks; ++m) {
        if (timed.enter[m] == clk::time_point{}) continue;
        transit.push_back(us(cb_end[m], timed.enter[m]));
        work.push_back(us(timed.enter[m], timed.leave[m]));
        const double w = us(timed.leave[m], cb_start[m + 1]);
        wait.push_back(w);
        round.push_back(us(cb_end[m], timed.leave[m]));
        if (w < 0) ++late;
    }
    std::printf("{\"unit\":\"us\",\"hop\":%u,\"delay_frames\":%u,\"blocks\":%zu,\"late_blocks\":%zu,\"q_in_transit\":%s,\"worker\":%s,"
                "\"ready_to_playback\":%s,\"capture_to_ready\":%s,\"stats\":%s}\n",
                hop, config.delay_frames, blocks, late, summary(transit).c_str(), summary(work).c_str(), summary(wait).c_str(),
                summary(round).c_str(), stats_json(engine.statistics()).c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "run" && argc == 7) return run(argv);
        if (mode == "latency" && argc == 3) return latency(argv);
        if (mode == "stages" && argc == 3) return stages(argv);
        if (mode == "comm" && argc == 4) return comm(argv);
        std::fprintf(stderr, "usage: see header of notebook/kcs_bridge.cpp\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "kcs_bridge: %s\n", e.what());
        return 1;
    }
}
