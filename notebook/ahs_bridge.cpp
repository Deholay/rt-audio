// Offline driver for notebook/ahs_rt.ipynb. Runs the real rt-audio engine and
// AHS releases; all audio I/O is raw float32 files, results are one JSON line.
//
//   loop <release|passthrough> <src.f32> <path.f32> <g1> <g2> <t_step_s> <frames> <out_prefix> [clip|tanh]
//   latency <release> <frames>
//   resampler
//   stages <seconds>
//   comm <seconds>
#include <rt_audio/ahs.hpp>
#include <rt_audio/project2.hpp>
#include <rt_audio/project2/detail/engine_impl.hpp>
#include <rt_audio/project2/detail/worker_impl.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Same upstream + renaming as the v7 releases (identical inline definitions).
#define ahs rt_audio_ahs_v7_c0291dd1e
#define ahs_live rt_audio_ahs_live_v7_c0291dd1e
#include "live_pipeline.h"
#undef ahs_live
#undef ahs

namespace {

namespace up = ::rt_audio_ahs_v7_c0291dd1e;
namespace live = ::rt_audio_ahs_live_v7_c0291dd1e;
using clk = std::chrono::steady_clock;
constexpr double rate = 48000.0;

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

double us(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

// {"mean":..,"p50":..,"p99":..,"max":..,"n":..}
std::string summary(std::vector<double> v) {
    if (v.empty()) return "null";
    double sum = 0;
    for (double x : v) sum += x;
    std::sort(v.begin(), v.end());
    char buf[200];
    std::snprintf(buf, sizeof buf, "{\"mean\":%.4f,\"p50\":%.4f,\"p99\":%.4f,\"max\":%.3f,\"n\":%zu}",
                  sum / static_cast<double>(v.size()), v[v.size() / 2], v[v.size() * 99 / 100],
                  v.back(), v.size());
    return buf;
}

// Passthrough wired like a release session.
class PassthroughSession final : public rt_audio::ahs::Session {
public:
    explicit PassthroughSession(const rt_audio::StreamConfig& c)
        : engine_(c, processor_), worker_(engine_, handler_) {}
    rt_audio::AudioCallback& callback() noexcept override { return engine_; }
    void start() override { worker_.start(); }
    void stop() noexcept override { worker_.stop(); }
    void rethrow_if_failed() const override { worker_.rethrow_if_failed(); }
private:
    rt_audio::PassthroughProcessor processor_;
    rt_audio::Project2 engine_;
    rt_audio::NoopAnalysisHandler handler_;
    rt_audio::AnalysisWorker worker_;
};

std::unique_ptr<rt_audio::ahs::Session> open(const std::string& name, const rt_audio::StreamConfig& c) {
    if (name == "passthrough") return std::make_unique<PassthroughSession>(c);
    return rt_audio::ahs::make_session(name, c, 1.0F); // unity gain: loop gain set by path
}

bool is_split(const std::string& name) {
    for (const auto& r : rt_audio::ahs::releases()) {
        if (r.name == name || (name == "latest" && &r == &rt_audio::ahs::releases().back())) {
            return r.architecture == "split";
        }
    }
    return false;
}

// Closed loop at 48 kHz: mic = src + g(t) * (path * speaker), speaker = engine
// output clipped to +-1 (DAC). The speaker history seen by the path is one
// callback old (device buffering), so the loop is causal for any path.
int loop(char** a) {
    const std::string name = a[2];
    const auto src = load(a[3]);
    const auto path = load(a[4]);
    const double g1 = std::pow(10.0, std::stod(a[5]) / 20.0), g2 = std::pow(10.0, std::stod(a[6]) / 20.0);
    const auto step = static_cast<long>(std::stod(a[7]) * rate);
    const auto frames = static_cast<std::uint32_t>(std::stoi(a[8]));
    const std::string out = a[9];
    // clip (default): mic = src + g*(path * clip(out)).
    // tanh (ahs_sim's speaker): mic = src + path * tanh(g*out) -- gain then soft saturation.
    const bool soft = std::string(a[10] ? a[10] : "clip") == "tanh";
    rt_audio::StreamConfig cfg{rate, 1, frames};
    auto session = open(name, cfg);
    const bool paced = is_split(name); // split needs its worker to keep up: run in real time
    const long n = static_cast<long>(src.size()) / frames * frames;
    std::vector<float> mic(n), spk(n), drv(n), cb_us;
    cb_us.reserve(static_cast<std::size_t>(n / frames));
    session->start();
    auto next = clk::now();
    for (long s = 0; s < n; s += frames) {
        const double g = s < step ? g1 : g2;
        for (long i = s; i < s + frames; ++i) {
            double fb = 0;
            const long newest = s - 1; // speaker samples available: up to previous callback
            for (std::size_t k = 0; k < path.size(); ++k) {
                const long j = i - static_cast<long>(frames) - static_cast<long>(k);
                if (j < 0) break;
                if (j <= newest) fb += path[k] * drv[static_cast<std::size_t>(j)];
            }
            mic[static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(i)] + static_cast<float>(soft ? fb : g * fb);
        }
        if (paced) { next += std::chrono::nanoseconds(static_cast<long>(1e9 * frames / rate)); std::this_thread::sleep_until(next); }
        const auto t0 = clk::now();
        if (!session->callback().process_callback(&mic[static_cast<std::size_t>(s)], &spk[static_cast<std::size_t>(s)], frames)) {
            throw std::runtime_error("callback rejected");
        }
        cb_us.push_back(static_cast<float>(us(t0, clk::now())));
        for (long i = s; i < s + frames; ++i) {
            auto& y = spk[static_cast<std::size_t>(i)];
            y = std::isfinite(y) ? std::clamp(y, -1.0F, 1.0F) : 0.0F;
            drv[static_cast<std::size_t>(i)] = soft ? static_cast<float>(std::tanh((s < step ? g1 : g2) * y)) : y;
        }
    }
    session->stop();
    session->rethrow_if_failed();
    save(out + "_mic.f32", mic);
    save(out + "_spk.f32", spk);
    save(out + "_cb_us.f32", cb_us);
    const auto st = session->callback().statistics();
    std::vector<double> d(cb_us.begin(), cb_us.end());
    std::printf("{\"release\":\"%s\",\"split\":%s,\"paced\":%s,\"callbacks\":%llu,\"analysis_dropped\":%llu,"
                "\"commands_dropped\":%llu,\"commands_applied\":%llu,\"callback_us\":%s}\n",
                name.c_str(), paced ? "true" : "false", paced ? "true" : "false",
                static_cast<unsigned long long>(st.callbacks), static_cast<unsigned long long>(st.analysis_dropped),
                static_cast<unsigned long long>(st.commands_dropped), static_cast<unsigned long long>(st.commands_applied),
                summary(d).c_str());
    return 0;
}

// Audio-path latency of the code: impulse in (no feedback), output peak lag.
int latency(char** a) {
    const std::string name = a[2];
    const auto frames = static_cast<std::uint32_t>(std::stoi(a[3]));
    rt_audio::StreamConfig cfg{rate, 1, frames};
    auto session = open(name, cfg);
    const long total = 48000, at = 24000;
    std::vector<float> in(total, 0.0F), out(total, 0.0F);
    in[at] = 0.5F;
    session->start();
    for (long s = 0; s + frames <= total; s += frames) {
        (void)session->callback().process_callback(&in[static_cast<std::size_t>(s)], &out[static_cast<std::size_t>(s)], frames);
    }
    session->stop();
    long best = at;
    for (long i = at; i < total; ++i) if (std::fabs(out[static_cast<std::size_t>(i)]) > std::fabs(out[static_cast<std::size_t>(best)])) best = i;
    std::printf("{\"release\":\"%s\",\"frames\":%u,\"lag_samples\":%ld,\"lag_ms\":%.4f}\n", name.c_str(), frames, best - at, (best - at) / 48.0);
    return 0;
}

// Speex Q4 48->16->48 alone (the AHS FixedResampler pair), impulse lag.
int resampler() {
    live::FixedResampler down(48000, 16000), upr(16000, 48000);
    const long total = 48000, at = 24000;
    std::vector<float> in(total, 0.0F), mid, out;
    in[at] = 0.5F;
    for (long s = 0; s < total; s += 192) {
        std::array<float, 64> m{}; spx_uint32_t ni = 192, no = 64;
        down.process(&in[static_cast<std::size_t>(s)], ni, m.data(), no);
        std::array<float, 192> o{}; spx_uint32_t ni2 = no, no2 = 192;
        upr.process(m.data(), ni2, o.data(), no2);
        out.insert(out.end(), o.begin(), o.begin() + no2);
    }
    long best = at;
    for (long i = at; i < static_cast<long>(out.size()); ++i) if (std::fabs(out[static_cast<std::size_t>(i)]) > std::fabs(out[static_cast<std::size_t>(best)])) best = i;
    std::printf("{\"lag_samples\":%ld,\"lag_ms\":%.4f}\n", best - at, (best - at) / 48.0);
    return 0;
}

// Per-stage compute time per 64-sample hop (16 kHz), closed loop at 16 kHz with
// a rising 1 kHz tone so detection/notches are active. Same classes and order
// as the releases.
int stages(char** a) {
    const int hops = static_cast<int>(std::stod(a[2]) * 16000 / 64);
    auto cfg = live::LivePipeline::config();
    up::Afc afc(cfg.afc, cfg.sample_rate);
    up::NotchBank notch(cfg.notch, cfg.sample_rate);
    auto dcfg = cfg.detector;
    dcfg.frame_rate_hz = cfg.sample_rate / static_cast<float>(cfg.block_size);
    up::HowlingDetector det(dcfg, cfg.sample_rate);
    live::FixedResampler down(48000, 16000), upr(16000, 48000);
    std::vector<float> frame(256, 0.0F), freqs, growth, hist(static_cast<std::size_t>(hops) * 64 + 64, 0.0F);
    freqs.reserve(8); growth.reserve(8);
    std::vector<double> sdown, safc, snotch, sguard, sdet, supd, sup;
    std::uint32_t seed = 1; long t = 0; float fast = 0, slow = 0;
    std::array<float, 192> in48{}, o48{}; std::array<float, 64> mic{}, ref{}, s{};
    for (int h = 0; h < hops; ++h) {
        for (auto& x : in48) { seed = seed * 1664525u + 1013904223u; x = (static_cast<float>(seed >> 8) / 16777216.F - .5F) * .05F; }
        spx_uint32_t ni = 192, no = 64;
        auto t0 = clk::now(); down.process(in48.data(), ni, mic.data(), no); sdown.push_back(us(t0, clk::now()));
        for (int i = 0; i < 64; ++i, ++t) {
            const float tone = std::min(1.0F, static_cast<float>(t) / 160000.F) * .3F * std::sin(6.2831853F * 1000.F * static_cast<float>(t) / 16000.F);
            ref[static_cast<std::size_t>(i)] = t >= 128 ? hist[static_cast<std::size_t>(t - 128)] : 0.0F;
            mic[static_cast<std::size_t>(i)] += tone + .5F * ref[static_cast<std::size_t>(i)];
        }
        t0 = clk::now(); for (int i = 0; i < 64; ++i) s[i] = afc.process(mic[i], ref[i]); safc.push_back(us(t0, clk::now()));
        std::copy(frame.begin() + 64, frame.end(), frame.begin()); std::copy(s.begin(), s.end(), frame.end() - 64);
        t0 = clk::now(); for (int i = 0; i < 64; ++i) s[i] = notch.process(s[i]); snotch.push_back(us(t0, clk::now()));
        t0 = clk::now();
        for (int i = 0; i < 64; ++i) { const float v = std::abs(s[i]); fast = .99F * fast + .01F * v; slow = .9995F * slow + .0005F * v; }
        sguard.push_back(us(t0, clk::now()));
        for (int i = 0; i < 64; ++i) hist[static_cast<std::size_t>(t - 64 + i)] = s[i];
        t0 = clk::now(); const auto& c = det.analyze(frame.data()); sdet.push_back(us(t0, clk::now()));
        t0 = clk::now();
        freqs.clear(); growth.clear();
        for (const auto& x : c) { freqs.push_back(x.freq_hz); growth.push_back(x.growth_db_per_s); }
        notch.update(freqs, growth, 0.004F); afc.setFastAdapt(!freqs.empty()); afc.setSubtractEnabled(true);
        supd.push_back(us(t0, clk::now()));
        ni = 64; no = 192;
        t0 = clk::now(); upr.process(s.data(), ni, o48.data(), no); sup.push_back(us(t0, clk::now()));
    }
    std::printf("{\"unit\":\"us per 64-sample hop (4 ms audio)\",\"speex_down\":%s,\"afc\":%s,\"notch\":%s,"
                "\"guard_env\":%s,\"detector\":%s,\"apply_detection\":%s,\"speex_up\":%s}\n",
                summary(sdown).c_str(), summary(safc).c_str(), summary(snotch).c_str(), summary(sguard).c_str(),
                summary(sdet).c_str(), summary(supd).c_str(), summary(sup).c_str());
    return 0;
}

// Framework communication with a timestamp-only protocol on the real
// Engine/Worker; callback paced every 128/48000 s like a device.
std::int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count(); }
struct ProbeTap { std::uint64_t seq = 0; std::int64_t t_push = 0; std::array<float, 64> pad{}; };
struct ProbeCmd { std::uint64_t seq = 0; std::int64_t t_push = 0, t_pop = 0, t_submit = 0; };
struct ProbeProc { void prepare(const rt_audio::StreamConfig&) {} std::vector<double> push, pop, q1, work, q2, rt; std::uint64_t seq = 0; };
struct ProbeHandler { void prepare(const rt_audio::StreamConfig&) {} };
struct Probe {
    using Tap = ProbeTap; using Command = ProbeCmd; using Processor = ProbeProc; using Handler = ProbeHandler;
    using Sink = rt_audio::CommandSink<ProbeCmd>;
    static constexpr std::size_t tap_capacity = 64, command_capacity = 64;
    static bool valid(const ProbeCmd&, const rt_audio::StreamConfig&) noexcept { return true; }
    static std::uint64_t sequence(const ProbeCmd& c) noexcept { return c.seq; }
    static void deliver(ProbeHandler&, const ProbeTap& t, Sink& s) {
        ProbeCmd c{t.seq, t.t_push, now_ns(), 0}; c.t_submit = now_ns(); (void)s.try_submit(c);
    }
    void callback(rt_audio::CallbackPort<Probe>& port, ProbeProc& p, const rt_audio::ProcessBlock& b, bool) noexcept {
        std::copy_n(b.input, b.frames, b.output);
        auto t0 = clk::now(); ProbeCmd c;
        while (port.pop_command(c)) {
            const auto t = now_ns();
            if (p.q1.size() < p.q1.capacity()) {
                p.q1.push_back((c.t_pop - c.t_push) / 1e3); p.work.push_back((c.t_submit - c.t_pop) / 1e3);
                p.q2.push_back((t - c.t_submit) / 1e3); p.rt.push_back((t - c.t_push) / 1e3);
            }
        }
        if (p.pop.size() < p.pop.capacity()) p.pop.push_back(us(t0, clk::now()));
        ProbeTap tap; tap.seq = ++p.seq; tap.t_push = now_ns();
        t0 = clk::now(); (void)port.push_tap(tap);
        if (p.push.size() < p.push.capacity()) p.push.push_back(us(t0, clk::now()));
    }
};

int comm(char** a) {
    const int n = static_cast<int>(std::stod(a[2]) * rate / 128);
    ProbeProc p;
    for (auto* v : {&p.push, &p.pop, &p.q1, &p.work, &p.q2, &p.rt}) v->reserve(static_cast<std::size_t>(n));
    rt_audio::Engine<Probe> e(rt_audio::StreamConfig{rate, 1, 128}, p);
    ProbeHandler h; rt_audio::Worker<Probe> w(e, h); w.start();
    std::array<float, 128> in{}, out{};
    auto next = clk::now();
    for (int i = 0; i < n; ++i) {
        next += std::chrono::nanoseconds(2666667); std::this_thread::sleep_until(next);
        (void)e.process_callback(in.data(), out.data(), 128);
    }
    w.stop();
    const auto st = e.statistics();
    std::printf("{\"unit\":\"us\",\"analysis_dropped\":%llu,\"commands_dropped\":%llu,\"push_tap\":%s,\"drain_q2\":%s,"
                "\"q1_transit\":%s,\"worker\":%s,\"q2_transit\":%s,\"round_trip\":%s}\n",
                static_cast<unsigned long long>(st.analysis_dropped), static_cast<unsigned long long>(st.commands_dropped),
                summary(p.push).c_str(), summary(p.pop).c_str(), summary(p.q1).c_str(), summary(p.work).c_str(),
                summary(p.q2).c_str(), summary(p.rt).c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "loop" && (argc == 10 || argc == 11)) return loop(argv);
        if (mode == "latency" && argc == 4) return latency(argv);
        if (mode == "resampler" && argc == 2) return resampler();
        if (mode == "stages" && argc == 3) return stages(argv);
        if (mode == "comm" && argc == 3) return comm(argv);
        std::fprintf(stderr, "usage: see header of ahs_bridge.cpp\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ahs_bridge: %s\n", e.what());
        return 1;
    }
}
