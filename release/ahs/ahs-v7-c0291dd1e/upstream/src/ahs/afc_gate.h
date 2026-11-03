#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ahs {

// What a gate is allowed to look at when it decides whether the canceller's
// estimate should be subtracted from the microphone signal.
//
// The built-in gate uses `err_env / mic_env`, and the closed-loop measurements
// show that observation cannot separate "the canceller removed feedback" from
// "the canceller removed the talker": at short loop delays the reference is
// almost the talker's own speech, cancelling the talker also lowers the error
// envelope, and the gate opens onto a filter that is worse than no filter at
// all. Everything a candidate gate might use instead is therefore exposed here,
// including the current filter, so that alternatives can be evaluated against
// the same runs without touching the canceller.
struct AfcGateObservation {
  float err_env = 0.0f;        // smoothed |y - d_hat|, the unfaded error
  float mic_env = 0.0f;        // smoothed |y|
  float est_env = 0.0f;        // smoothed |d_hat|, the estimated feedback
  float est_to_mic = 0.0f;     // est_env / mic_env
  float beta_pre = 0.0f;       // subtraction factor before the gate
  bool fast_adapt = false;     // the engine is seeing howling
  const std::vector<float>* taps = nullptr;  // current path estimate
  long sample_index = 0;
};

// A gate returns the factor the subtraction is scaled by, in [0, 1].
//
// Called once per sample on the audio thread: implementations must not
// allocate, block or throw. The canceller keeps adapting regardless of what the
// gate returns, so a gate only decides what reaches the output.
class AfcGate {
 public:
  virtual ~AfcGate() = default;
  virtual float gain(const AfcGateObservation& obs) = 0;
  virtual void reset() {}
};

// Reference points that bracket the problem: subtract always, subtract never.
class AlwaysOnGate : public AfcGate {
 public:
  float gain(const AfcGateObservation&) override { return 1.0f; }
};

class AlwaysOffGate : public AfcGate {
 public:
  float gain(const AfcGateObservation&) override { return 0.0f; }
};

// Cheating gate: it is told the true feedback path and opens only while the
// filter is actually close to it. It is an evaluation instrument, never part of
// a delivered configuration, because nothing running in a room can measure its
// own misalignment.
//
// It is a ceiling for decision rules of the form "subtract while the estimate
// is accurate", and only for those: it is not an upper bound on the score a
// case can reach. Subtracting changes the loop, so each strategy runs a
// different trajectory, and the measurements show a poor estimate can still be
// worth subtracting because it removes loop gain (`results/gate_sweep.md`).
//
// It starts closed. At the first sample the filter is all-zero, which is 0 dB
// of misalignment, so "open only below the threshold" means closed for every
// threshold at or below 0 dB.
class OracleGate : public AfcGate {
 public:
  OracleGate(const std::vector<float>& true_path, float threshold_db,
             float ramp_rate = 2e-5f, int eval_interval = 64)
      : truth_(true_path), threshold_db_(threshold_db), ramp_(ramp_rate),
        interval_(eval_interval) {
    truth_energy_ = 0.0;
    for (float v : truth_) truth_energy_ += static_cast<double>(v) * v;
  }

  float gain(const AfcGateObservation& obs) override {
    if (obs.taps && --countdown_ <= 0) {
      countdown_ = interval_;
      open_ = misalignmentDb(*obs.taps) < threshold_db_;
    }
    // Same ramp rate as the built-in gate, so that the comparison is between
    // the observations the two use and not between their switching speeds.
    gain_ += open_ ? ramp_ : -ramp_;
    gain_ = std::max(0.0f, std::min(1.0f, gain_));
    return gain_;
  }

  void reset() override {
    gain_ = 0.0f;
    open_ = false;
    countdown_ = 0;
  }

  double misalignmentDb(const std::vector<float>& est) const {
    double num = 0.0;
    const std::size_t n = std::max(est.size(), truth_.size());
    for (std::size_t k = 0; k < n; ++k) {
      const double truth = k < truth_.size() ? truth_[k] : 0.0;
      const double err = truth - (k < est.size() ? est[k] : 0.0);
      num += err * err;
    }
    return 10.0 * std::log10((num + 1e-30) / (truth_energy_ + 1e-30));
  }

 private:
  std::vector<float> truth_;
  double truth_energy_ = 0.0;
  float threshold_db_;
  float ramp_;
  int interval_;
  int countdown_ = 0;
  bool open_ = false;
  float gain_ = 0.0f;
};

}  // namespace ahs
