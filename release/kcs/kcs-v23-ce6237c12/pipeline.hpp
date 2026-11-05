#pragma once

// C++ port of KCS v23 at ce6237c124a62c79b28667aced2ee2ff6df6b1b0:
//   upstream/scripts/item3_transient_v23.py  TransientAR32 (16 kHz core)
//   upstream/scripts/item3_bridge48k.py      Bridge48k (48 kHz transport)
//   upstream/scripts/item3_rt_host.py        CausalFIR, rate_taps
// float64 arithmetic like NumPy. Not bit-identical (BLAS/LAPACK summation and
// eigen solver differ); see PROVENANCE.md for the measured tolerance.
// No allocation after construction; reset() restores a fresh instance.

#include <array>
#include <cstdint>

namespace rt_audio::kcs::v23_ce6237c12 {

struct Counters {
    std::uint64_t triggers = 0, low_energy = 0, pole_projection = 0, projected_roots = 0,
                  solve_failure = 0, nonfinite_prediction = 0, warmup_samples = 0, bypass = 0;
};

struct Event {
    std::uint64_t trigger_sample = 0; // 16 kHz core clock
    std::uint32_t window = 0;         // W: 128 or 256
    double r_db = 0.0;
    double s_dbfs = 0.0;
    bool bypassed = false;
};

// TransientAR32: push one 16 kHz sample, get the sample emitted 144 ticks earlier.
class Core16k {
public:
    static constexpr int latency_samples = 144;
    static constexpr int short_len = 32, long_len = 320, n_train = 640, b = 64, order = 32,
                         c = 16, d = 16, refractory = 128, w_short = 128, w_long = 256;
    static constexpr double t_db = 8.0, r_t = 20.0, s_t = -5.0;

    Core16k() noexcept;
    void reset() noexcept;
    double tick(double value);
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    // Last trigger; valid when counters().triggers > 0.
    [[nodiscard]] const Event& last_event() const noexcept { return last_event_; }

private:
    static constexpr int raw_len = n_train + b;    // 704
    static constexpr int z_len = short_len + long_len; // 352
    static constexpr int max_repair = b + w_long;  // 320

    void start_repair(std::int64_t t, double ratio, double short_energy);
    bool fit(int length);
    [[nodiscard]] double weight(int j) const noexcept;

    std::array<double, 33> fir_{};
    std::array<double, 33> fir_input_{}; // ring, head = oldest
    std::array<double, z_len> z_{};      // ring
    std::array<double, raw_len> raw_{};  // ring
    std::array<double, latency_samples> fifo_{}; // ring, head = next emitted
    int fir_head_ = 0, z_head_ = 0, z_count_ = 0, raw_head_ = 0, raw_count_ = 0, fifo_head_ = 0;
    std::int64_t clock_ = 0, above_ = 0, next_trigger_ = 0;
    std::int64_t event_start_ = -(b + w_short), previous_end_ = -(b + w_short);
    int repair_length_ = b + w_short;
    bool have_replacement_ = false;
    std::array<double, max_repair> replacement_{};
    Counters counters_{};
    Event last_event_{};
};

// Bridge48k process_block semantics, one 48 kHz sample at a time on a global
// phase: y[n] corresponds to x[n - delay_frames].
class Pipeline48k {
public:
    static constexpr std::uint32_t fir_taps = 61;
    static constexpr std::uint32_t delay_frames = 30 + 3 * Core16k::latency_samples + 30; // 492

    Pipeline48k() noexcept;
    void reset() noexcept;
    float tick(float x);
    [[nodiscard]] const Core16k& core() const noexcept { return core_; }

private:
    static double filter(const std::array<double, fir_taps>& taps,
                         std::array<double, fir_taps>& history, int& head, double x) noexcept;
    std::array<double, fir_taps> down_taps_{}, up_taps_{}; // up = 3 * down, as in KCS
    std::array<double, fir_taps> down_{}, up_{}; // rings, head = oldest
    int down_head_ = 0, up_head_ = 0;
    std::uint64_t position_ = 0;
    Core16k core_;
};

} // namespace rt_audio::kcs::v23_ce6237c12
