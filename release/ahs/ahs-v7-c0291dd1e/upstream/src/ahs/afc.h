#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include <memory>
#include <stdexcept>
#include "ahs/pbfd_kalman.h"

#ifndef AHS_AFC_CANDIDATE
#define AHS_AFC_CANDIDATE 0
#endif

#include "ahs/afc_gate.h"

namespace ahs {

struct AfcConfig {
  // 0 preserves delivered NLMS; 1/2 are preregistered research candidates.
  int candidate = AHS_AFC_CANDIDATE;
  double kalman_q_fraction = 2e-5;  // zero only for the preregistered A8-prime probe
  bool probe_gate_bypass = false;
  int filter_taps = 256;      // 16 ms of feedback path @ 16 kHz
  // Optional measured lower bound on the reference-to-microphone path.
  // The reference is the signal currently driving the loudspeaker, so forward
  // I/O buffering is not part of this delay. A positive value pins earlier
  // taps to zero; it must not exclude the actual acoustic arrival.
  int min_delay_taps = 0;
  int ar_order = 20;            // prediction-error-method whitening order
  int ar_update_samples = 512;   // AR / pitch analysis window (32 ms @ 16 kHz)
  float ar_smoothing = 0.7f;     // inertia on the AR coefficients between updates
  float ar_min_prediction_gain_db = 2.0f;  // below this the signal is already white
  // Long-term (pitch) prediction. Short-term AR whitening removes the spectral
  // envelope but leaves the harmonic comb of voiced speech, and that periodic
  // structure is precisely what biases a closed-loop canceller: the reference is
  // a delayed copy of the talker, so a periodic talker looks like a valid
  // feedback path at multiples of the pitch period. Cascading P(z) = 1 - b z^-T
  // with the AR filter removes it.
  bool long_term_prediction = true;
  int ltp_min_lag = 40;    // 400 Hz
  int ltp_max_lag = 250;   // 64 Hz
  float ltp_max_gain = 0.7f;
  float ltp_min_correlation = 0.25f;
  // Two-speed NLMS. Step size trades convergence speed against misadjustment,
  // and misadjustment noise lands straight in the output: a large step buys
  // maximum stable gain but costs output SNR even when nothing is howling.
  // So the canceller runs slow and accurate by default and is switched to the
  // fast step only while the engine sees howling or a runaway loop.
  float mu = 0.003f;       // NLMS step size, steady state
  float mu_fast = 0.05f;   // NLMS step size while howling is present
  float mu_ramp_ms = 20.0f;  // smooth transition between the two
  // NLMS regularisation. An absolute floor alone makes the effective step size
  // level dependent (a quiet talker adapts far slower than a loud one), so the
  // regularisation is a fraction of the running reference energy with the
  // absolute value only as a floor for silence.
  float delta = 1e-9f;
  float delta_relative = 0.01f;
  // Cancellation is also faded in with the estimated feedback-to-microphone
  // ratio, so a filter that has nothing to cancel cannot inject its residual.
  float full_cancel_ratio = 0.3f;  // d_hat/y ratio at which cancellation is full
  float ratio_smoothing = 0.999f;  // envelope pole for that ratio
  // Benefit gate: subtracting an estimate that is wrong makes the output worse
  // than doing nothing, which is what a badly biased AFC does in a closed loop.
  // The canceller is therefore backed off whenever its own error envelope is not
  // below the microphone envelope by `min_benefit`.
  bool benefit_gate = true;
  // Two thresholds: while nothing is howling the canceller has to show a clear
  // improvement to be allowed to touch the signal, because the cost of being
  // wrong is pure damage to clean speech. Once howling is present the trade is
  // the other way round and a marginal canceller is still worth having.
  float min_benefit = 0.9f;           // e_env/y_env above this counts as no benefit
  float min_benefit_howling = 0.999f;
  float benefit_rate = 2e-5f;  // per-sample ramp of the gate (~1.5 s full swing)
  // Leakage pulls the filter back towards zero whenever the data does not
  // actively support its taps. Without it the canceller keeps fitting whatever
  // correlation it can find while there is no real feedback, ends up worse than
  // no filter at all, and then damages the signal the moment it is switched in.
  float leakage = 3e-5f;
  float activity_threshold = 1e-6f;  // freeze adaptation on a silent loudspeaker
  // Divergence guard: the closed loop can push NLMS into a runaway state, and a
  // diverged canceller is far worse than none at all.
  float max_tap_norm = 100.0f;
  // |e| vs |y| ratio that counts as divergence. A canceller that makes the
  // signal louder than the microphone is by definition not cancelling, so the
  // useful range is a small factor; the previous 1000 (60 dB) let a diverging
  // canceller emit a 20000x burst before it tripped.
  float max_error_growth = 4.0f;

  // Safety band. The divergence guard above only trips once the canceller is
  // already emitting four times the microphone level, by which point the loop
  // it feeds has had seconds to run away (T028: block ERLE -9.5 dB well before
  // the output reaches 258x its source). Block ERLE below 0 dB says the same
  // thing much earlier and without ambiguity: the estimate is worse than no
  // estimate, so subtracting it adds energy to the loop instead of removing it.
  // A single bad block is normal -- ERLE is noisy at block rate, and any
  // canceller is briefly wrong after a path change -- so it takes a run of
  // them. On the run, adaptation freezes (the gradient is what is driving the
  // filter to the wrong place) and the taps leak to zero, which walks the
  // canceller back to the one estimate that cannot be worse than nothing.
  //
  // Off by default: adjudicated after the item 2 matrix. The band does what it
  // was specified to do on T028 (206.70x -> 1.00x) but fails both acceptance
  // lines -- runs above 10x bypass fall 53 -> 20 rather than to zero, and 54 of
  // the 129 howling runs get worse, the worst by 38.08 dB, because the freeze
  // is not reversible. Block ERLE is a wideband ratio: a loop can be unstable
  // at one frequency while the canceller is still removing energy overall, so
  // the criterion misses exactly the runs the acceptance line was about. See
  // results/v8_item2/README.md. Enable with --afc-safety to reproduce that
  // matrix; the delivered configuration does not use it.
  bool safety_band = false;
  int erle_block_samples = 64;   // one engine block @ 16 kHz
  // Consecutive blocks below 0 dB before freezing; 8 blocks is 32 ms. From the
  // sweep in results/v8_item2/sweep: 4 and 8 both take T028's peak down to the
  // level of switching the AFC off, 16 and 32 react late enough to leave a
  // 1.12x peak, and 8 engages on fewer of the runs whose peak the AFC is not
  // responsible for than 4 does.
  int erle_bad_blocks = 8;
  // Per-sample decay of the taps while frozen; 1e-3 is a 62 ms time constant,
  // long enough not to click and short enough to be gone within a block or two
  // of the detector's own reaction time.
  float safety_leak = 1e-3f;
  // Fraction of the tap norm at engage time at which adaptation would resume.
  // Zero, i.e. the freeze holds until the engine is reset. Re-converging after
  // the leak looked like the better trade -- freezing permanently gives up
  // cancellation for the rest of the session -- but measured the other way:
  // with release enabled the peak became a non-monotonic function of both
  // knobs (T028 at 313x with N=4, worse than the 207x it is meant to fix),
  // because the loop is still marginally stable when adaptation restarts and
  // the filter re-diverges. The freeze/re-adapt cycle is the thing that hurts,
  // not the freeze. Set it non-zero only with a sweep to back it.
  float safety_release_fraction = 0.0f;
};

// Adaptive feedback canceller: NLMS with prediction-error-method (PEM) whitening.
// Whitening decorrelates the loudspeaker signal from the source signal, which is
// what makes AFC converge in a closed loop -- unlike AEC, the "reference" here
// originates from the target speech itself, so plain NLMS would be biased.
class Afc {
 public:
  explicit Afc(const AfcConfig& cfg, float sample_rate = 16000.0f)
      : cfg_(cfg),
        n_(static_cast<std::size_t>(cfg.filter_taps)),
        h_(n_, 0.0f),
        x_hist_(n_, 0.0f),
        xw_hist_(n_, 0.0f),
        a_(static_cast<std::size_t>(cfg.ar_order) + 1, 0.0f),
        x_ar_(static_cast<std::size_t>(cfg.ar_order) + 1, 0.0f),
        e_ar_(static_cast<std::size_t>(cfg.ar_order) + 1, 0.0f),
        e_buf_(static_cast<std::size_t>(cfg.ar_update_samples), 0.0f),
        r_scratch_(static_cast<std::size_t>(cfg.ar_order) + 1, 0.0),
        a_scratch_(static_cast<std::size_t>(cfg.ar_order) + 1, 0.0),
        tmp_scratch_(static_cast<std::size_t>(cfg.ar_order) + 1, 0.0),
        x_ltp_(static_cast<std::size_t>(std::max(1, cfg.ltp_max_lag) + 1), 0.0f),
        e_ltp_(static_cast<std::size_t>(std::max(1, cfg.ltp_max_lag) + 1), 0.0f),
        ltp_dec_(static_cast<std::size_t>(cfg.ar_update_samples / 2), 0.0f) {
    if (cfg.candidate < 0 || cfg.candidate > 2) throw std::invalid_argument("AFC candidate must be 0, 1 or 2");
    if (cfg.candidate && cfg.safety_band) throw std::invalid_argument("Kalman candidates do not support legacy safety band");
    a_[0] = 1.0f;
    first_tap_ = static_cast<std::size_t>(std::max(0, std::min(cfg.min_delay_taps, cfg.filter_taps - 1)));
    if (cfg.candidate) kalman_ = std::make_unique<PbfdKalman>(n_, first_tap_, a_.size() - 1, static_cast<std::size_t>(cfg.ltp_max_lag), cfg.kalman_q_fraction);
    mu_now_ = cfg.mu;
    mu_step_ = 1.0f / std::max(1.0f, 0.001f * cfg.mu_ramp_ms * sample_rate);
  }

  // Called by the engine once per block: true while howling or a runaway loop
  // is present, i.e. while fast tracking is worth its misadjustment cost.
  void setFastAdapt(bool fast) { fast_target_ = fast; }

  // Switches the subtraction (not the adaptation) in and out, smoothly. While it
  // is out the filter keeps tracking the path but the microphone signal passes
  // through untouched, so a converged canceller costs nothing when there is no
  // feedback to cancel.
  void setSubtractEnabled(bool on) { subtract_target_ = on ? 1.0f : 0.0f; }

  // Replaces the built-in benefit gate with an external decision rule. The
  // pointer is owned by the caller and must outlive the canceller. Nullptr,
  // the default, keeps the built-in gate, so the delivered configuration is
  // unaffected by this hook.
  void setGate(AfcGate* gate) { gate_ = gate; }

  // Smoothed ratio between the estimated feedback and the microphone signal.
  // The engine uses it as its feedback-present evidence.
  float feedbackToMicRatio() const { return d_env_ / (y_env_ + 1e-9f); }

  // Diagnostics for offline analysis; not used by the audio path.
  double diagErleDb() const {
    return 10.0 * std::log10((diag_y_power_ + 1e-30) / (diag_e_power_ + 1e-30));
  }
  double diagMeanBeta() const {
    return diag_samples_ ? diag_beta_sum_ / static_cast<double>(diag_samples_) : 0.0;
  }
  // Safety band state, for the per-block traces and the matrix report.
  bool safetyFrozen() const { return safety_frozen_; }

  // True when the last sample actually took the gradient update, i.e. the
  // canceller is tracking rather than parked on silence or frozen. This is the
  // condition the decorrelation shifter's AFC-active gate reads.
  bool adaptingNow() const { return adapting_; }
  int safetyFreezeCount() const { return safety_freezes_; }
  double safetyLastBlockErleDb() const { return safety_last_block_erle_db_; }

  // Instantaneous gate state, for the per-block traces.
  float diagGateGain() const { return benefit_; }
  float diagLastBeta() const { return last_beta_; }
  float diagErrEnv() const { return e_env_; }
  float diagMicEnv() const { return y_env_; }
  float diagEstEnv() const { return d_env_; }
  const std::vector<float>& coefficients() const { return h_; }

  // y: microphone sample, x: sample currently driving the loudspeaker.
  // Returns the feedback-compensated microphone sample.
  float process(float y, float x) {
    if (kalman_) return processKalman(y, x);
    const float mu_target = fast_target_ ? cfg_.mu_fast : cfg_.mu;
    mu_now_ += std::max(-mu_step_ * cfg_.mu_fast, std::min(mu_step_ * cfg_.mu_fast, mu_target - mu_now_));

    // Circular history: index 0 of the logical delay line is at `pos_`.
    pos_ = (pos_ == 0) ? n_ - 1 : pos_ - 1;
    const float xw_old = xw_hist_[pos_];
    x_hist_[pos_] = x;

    float d_hat = 0.0f;
    for (std::size_t i = first_tap_; i < n_; ++i) {
      std::size_t j = pos_ + i;
      if (j >= n_) j -= n_;
      d_hat += h_[i] * x_hist_[j];
    }
    // Fade the canceller in with the estimated feedback-to-microphone ratio.
    d_env_ = cfg_.ratio_smoothing * d_env_ + (1.0f - cfg_.ratio_smoothing) * std::abs(d_hat);
    const float ratio = d_env_ / (y_env_ + 1e-9f);
    float beta = std::min(1.0f, ratio / std::max(1e-6f, cfg_.full_cancel_ratio));
    subtract_gate_ += std::max(-kSubtractStep, std::min(kSubtractStep, subtract_target_ - subtract_gate_));
    beta *= subtract_gate_;
    if (gate_ != nullptr) {
      AfcGateObservation obs;
      obs.err_env = e_env_;
      obs.mic_env = y_env_;
      obs.est_env = d_env_;
      obs.est_to_mic = ratio;
      obs.beta_pre = beta;
      obs.fast_adapt = fast_target_;
      obs.taps = &h_;
      obs.sample_index = diag_samples_;
      benefit_ = std::max(0.0f, std::min(1.0f, gate_->gain(obs)));
      beta *= benefit_;
    } else if (cfg_.benefit_gate) {
      const float min_benefit = fast_target_ ? cfg_.min_benefit_howling : cfg_.min_benefit;
      const bool helping = e_env_ < min_benefit * y_env_ || y_env_ < 1e-7f;
      benefit_ += helping ? cfg_.benefit_rate : -cfg_.benefit_rate;
      benefit_ = std::max(0.0f, std::min(1.0f, benefit_));
      beta *= benefit_;
    }
    last_beta_ = beta;
    const float e = y - beta * d_hat;

    // The filter itself must keep tracking the true path, so it adapts on the
    // full error, not on the faded one.
    const float e_full = y - d_hat;
    diag_y_power_ += static_cast<double>(y) * y;
    diag_e_power_ += static_cast<double>(e_full) * e_full;
    diag_beta_sum_ += beta;
    ++diag_samples_;

    float xw = arFilter(x_ar_, x);
    float ew = arFilter(e_ar_, e_full);
    if (cfg_.long_term_prediction) {
      const float xw_lag = ltpDelayed(x_ltp_, x_ltp_pos_);
      const float ew_lag = ltpDelayed(e_ltp_, e_ltp_pos_);
      pushLtp(x_ltp_, x_ltp_pos_, xw);
      pushLtp(e_ltp_, e_ltp_pos_, ew);
      xw -= ltp_gain_ * xw_lag;
      ew -= ltp_gain_ * ew_lag;
    }
    xw_hist_[pos_] = xw;

    // Running energy of the whitened reference. Kept in double and rebuilt
    // periodically: a float sliding sum drifts badly over minutes of audio.
    x_energy_ += static_cast<double>(xw) * xw - static_cast<double>(xw_old) * xw_old;
    if (++energy_refresh_ >= kEnergyRefresh) {
      energy_refresh_ = 0;
      double acc = 0.0;
      for (float v : xw_hist_) acc += static_cast<double>(v) * v;
      x_energy_ = acc;
    }
    if (x_energy_ < 0.0) x_energy_ = 0.0;

    if (cfg_.safety_band) updateSafetyBand(y, e_full);

    adapting_ = false;
    if (safety_frozen_) {
      // Frozen: no gradient, taps decaying. The subtraction is left in place so
      // that what the listener hears is the cancellation fading out rather than
      // being cut, and it fades to nothing because the taps do.
      const float keep = 1.0f - cfg_.safety_leak;
      for (std::size_t i = first_tap_; i < n_; ++i) h_[i] *= keep;
    } else if (x_energy_ > static_cast<double>(cfg_.activity_threshold)) {
      adapting_ = true;  // set where the gradient is actually applied
      x_energy_avg_ = 0.9999 * x_energy_avg_ + 0.0001 * x_energy_;
      const double reg = std::max(static_cast<double>(cfg_.delta),
                                  static_cast<double>(cfg_.delta_relative) * x_energy_avg_);
      const float g = static_cast<float>(static_cast<double>(mu_now_) * ew / (x_energy_ + reg));
      const float keep = 1.0f - cfg_.leakage;
      for (std::size_t i = first_tap_; i < n_; ++i) {
        std::size_t j = pos_ + i;
        if (j >= n_) j -= n_;
        h_[i] = keep * h_[i] + g * xw_hist_[j];
      }
    }

    // Divergence check on smoothed levels (instantaneous ratios are meaningless
    // during zero crossings), and on the tap norm every `kNormCheck` samples.
    y_env_ = 0.999f * y_env_ + 0.001f * std::abs(y);
    e_env_ = 0.999f * e_env_ + 0.001f * std::abs(e_full);
    const bool level_runaway = e_env_ > cfg_.max_error_growth * (y_env_ + 1e-9f);
    bool norm_runaway = false;
    if (++norm_check_ >= kNormCheck) {
      norm_check_ = 0;
      norm_runaway = tapNorm() > cfg_.max_tap_norm;
    }
    if (!std::isfinite(e) || level_runaway || norm_runaway) {
      adapting_ = false;
      reset();
      return y;
    }

    e_buf_[e_pos_] = e_full;
    e_pos_ = (e_pos_ + 1) % e_buf_.size();
    if (++since_ar_update_ >= static_cast<int>(e_buf_.size())) {
      since_ar_update_ = 0;
      updateArModel();
      if (cfg_.long_term_prediction) updateLongTermModel();
    }
    return e;
  }

  const std::vector<float>& taps() const { return h_; }

  // Whitening filter A(z) currently in use (a_[0] == 1).
  const std::vector<float>& arCoefficients() const { return a_; }

  void reset() {
    if (kalman_) {
      kalman_->reset(); confidence_gate_ = 0; confidence_open_ = false;
      e_pos_ = static_cast<std::size_t>(kalman_samples_ % e_buf_.size());
      since_ar_update_ = static_cast<int>(e_pos_);
    }
    std::fill(h_.begin(), h_.end(), 0.0f);
    std::fill(x_hist_.begin(), x_hist_.end(), 0.0f);
    std::fill(xw_hist_.begin(), xw_hist_.end(), 0.0f);
    std::fill(x_ar_.begin(), x_ar_.end(), 0.0f);
    std::fill(e_ar_.begin(), e_ar_.end(), 0.0f);
    std::fill(e_buf_.begin(), e_buf_.end(), 0.0f);
    std::fill(a_.begin(), a_.end(), 0.0f);
    a_[0] = 1.0f;
    std::fill(x_ltp_.begin(), x_ltp_.end(), 0.0f);
    std::fill(e_ltp_.begin(), e_ltp_.end(), 0.0f);
    x_ltp_pos_ = e_ltp_pos_ = 0;
    ltp_lag_ = 0;
    ltp_gain_ = 0.0f;
    x_energy_ = x_energy_avg_ = 0.0;
    y_env_ = e_env_ = d_env_ = 0.0f;
    mu_now_ = cfg_.mu;
    benefit_ = 1.0f;
    if (gate_ != nullptr) gate_->reset();
    subtract_gate_ = subtract_target_;
    safety_frozen_ = false;
    safety_bad_blocks_ = safety_block_samples_ = 0;
    safety_y_power_ = safety_e_power_ = 0.0;
    safety_last_block_erle_db_ = 0.0;
    ++resets_;
  }

  int resetCount() const { return resets_; }
  const PbfdKalman* kalmanDiagnostics() const { return kalman_.get(); }

 private:
  float processKalman(float y, float x) {
    ++kalman_samples_;
    if (!std::isfinite(y) || !std::isfinite(x)) {
      // Advance the estimator's absolute clock even on invalid standalone input.
      kalman_->push(0, 0, a_, ltp_lag_, ltp_gain_, h_);
      reset(); return std::isfinite(y) ? y : 0;
    }
    pos_ = pos_ == 0 ? n_ - 1 : pos_ - 1;
    x_hist_[pos_] = x;
    float d_hat = 0;
    for (std::size_t i = first_tap_; i < n_; ++i) {
      std::size_t j = pos_ + i; if (j >= n_) j -= n_;
      d_hat += h_[i] * x_hist_[j];
    }
    d_env_ = cfg_.ratio_smoothing * d_env_ + (1 - cfg_.ratio_smoothing) * std::abs(d_hat);
    const float ratio = d_env_ / (y_env_ + 1e-9f);
    subtract_gate_ += std::clamp(subtract_target_ - subtract_gate_, -kSubtractStep, kSubtractStep);
    float beta = 1;
    if (!cfg_.probe_gate_bypass) {
      if (cfg_.candidate == 2) {
        confidence_gate_ += std::clamp((confidence_open_ ? 1.0f : 0.0f) - confidence_gate_, -kSubtractStep, kSubtractStep);
        benefit_ = confidence_gate_;
        beta = subtract_gate_ * confidence_gate_;
      } else {
        beta = std::min(1.0f, ratio / std::max(1e-6f, cfg_.full_cancel_ratio)) * subtract_gate_;
        if (gate_ != nullptr) {
          AfcGateObservation obs;
          obs.err_env=e_env_; obs.mic_env=y_env_; obs.est_env=d_env_;
          obs.est_to_mic=ratio; obs.beta_pre=beta; obs.fast_adapt=fast_target_;
          obs.taps=&h_; obs.sample_index=diag_samples_;
          benefit_=std::clamp(gate_->gain(obs),0.0f,1.0f); beta*=benefit_;
        } else if (cfg_.benefit_gate) {
          const float threshold = fast_target_ ? cfg_.min_benefit_howling : cfg_.min_benefit;
          benefit_ = std::clamp(benefit_ + ((e_env_ < threshold*y_env_ || y_env_<1e-7f) ? cfg_.benefit_rate : -cfg_.benefit_rate),0.0f,1.0f);
          beta *= benefit_;
        }
      }
    }
    last_beta_ = beta;
    const float out = y - beta*d_hat, full_error = y-d_hat;
    diag_y_power_ += static_cast<double>(y)*y;
    diag_e_power_ += static_cast<double>(full_error)*full_error;
    diag_beta_sum_ += beta; ++diag_samples_;
    // The current PEM remains fixed for every window in this update.
    const bool updated = kalman_->push(y,x,a_,cfg_.long_term_prediction ? ltp_lag_ : 0,
                                      cfg_.long_term_prediction ? ltp_gain_ : 0,h_);
    adapting_ = kalman_->active();
    y_env_ = 0.999f*y_env_ + 0.001f*std::abs(y);
    e_env_ = 0.999f*e_env_ + 0.001f*std::abs(full_error);
    if (updated && cfg_.candidate == 2) {
      const double conf=kalman_->confidence();
      const float r=feedbackToMicRatio();
      if (conf>=0.90 && r>=1e-4f) confidence_open_=true;
      else if (conf<0.80 || r<5e-5f) confidence_open_=false;
    }
    bool norm_runaway=false;
    if (++norm_check_>=kNormCheck) { norm_check_=0; norm_runaway=tapNorm()>cfg_.max_tap_norm; }
    if (!std::isfinite(out) || !kalman_->valid() || norm_runaway ||
        e_env_>cfg_.max_error_growth*(y_env_+1e-9f)) {
      adapting_=false; reset(); return y;
    }
    e_buf_[e_pos_]=full_error; e_pos_=(e_pos_+1)%e_buf_.size();
    if (++since_ar_update_>=static_cast<int>(e_buf_.size())) {
      since_ar_update_=0; updateArModel();
      if (cfg_.long_term_prediction) updateLongTermModel();
    }
    return out;
  }
  std::unique_ptr<PbfdKalman> kalman_;
  std::size_t kalman_samples_ = 0;
  float confidence_gate_ = 0;
  bool confidence_open_ = false;
  // Block ERLE against the unfaded error, i.e. what the filter would do if it
  // were switched fully in. Measuring the faded error would let the fade hide a
  // filter that is already wrong, and the fade is not what feeds the loop back.
  void updateSafetyBand(float y, float e_full) {
    safety_y_power_ += static_cast<double>(y) * y;
    safety_e_power_ += static_cast<double>(e_full) * e_full;
    if (++safety_block_samples_ < cfg_.erle_block_samples) return;
    const double y_power = safety_y_power_, e_power = safety_e_power_;
    const double erle_db = 10.0 * std::log10((y_power + 1e-30) / (e_power + 1e-30));
    safety_last_block_erle_db_ = erle_db;
    safety_block_samples_ = 0;
    safety_y_power_ = safety_e_power_ = 0.0;

    if (safety_frozen_) {
      // A zero fraction means never: without the explicit test the taps
      // eventually underflow to zero and satisfy the comparison by accident.
      if (cfg_.safety_release_fraction > 0.0f &&
          tapNorm() <= cfg_.safety_release_fraction * safety_norm_at_freeze_) {
        safety_frozen_ = false;
        safety_bad_blocks_ = 0;
      }
      return;
    }
    // A silent block carries no evidence either way: y and e are both at the
    // numerical floor and their ratio is noise, so it neither accuses nor
    // exonerates the filter, and the counter is left where it is.
    if (y_power < static_cast<double>(cfg_.activity_threshold) ||
        !std::isfinite(erle_db)) {
      return;
    }
    safety_bad_blocks_ = (erle_db < 0.0) ? safety_bad_blocks_ + 1 : 0;
    if (safety_bad_blocks_ >= cfg_.erle_bad_blocks) {
      safety_frozen_ = true;
      safety_norm_at_freeze_ = std::max(tapNorm(), 1e-12f);
      ++safety_freezes_;
    }
  }

  float tapNorm() const {
    float n = 0.0f;
    for (float v : h_) n += v * v;
    return std::sqrt(n);
  }

  // FIR filtering by A(z) = 1 + a1 z^-1 + ... (the AR inverse / whitening filter).
  float arFilter(std::vector<float>& state, float in) const {
    for (std::size_t i = state.size() - 1; i > 0; --i) state[i] = state[i - 1];
    state[0] = in;
    float out = 0.0f;
    for (std::size_t i = 0; i < a_.size(); ++i) out += a_[i] * state[i];
    return out;
  }

  // Circular buffers for the long-term predictor, written after the value at
  // the current lag has been read.
  float ltpDelayed(const std::vector<float>& buf, std::size_t pos) const {
    if (ltp_lag_ <= 0) return 0.0f;
    std::size_t j = pos + buf.size() - static_cast<std::size_t>(ltp_lag_);
    while (j >= buf.size()) j -= buf.size();
    return buf[j];
  }

  static void pushLtp(std::vector<float>& buf, std::size_t& pos, float v) {
    buf[pos] = v;
    pos = (pos + 1 == buf.size()) ? 0 : pos + 1;
  }

  // Pitch lag and gain from the normalised autocorrelation of the recent error,
  // searched on a 2:1 decimated copy and then refined, to keep the cost down.
  void updateLongTermModel() {
    const int n = static_cast<int>(e_buf_.size());
    const int half = n / 2;
    for (int i = 0; i < half; ++i) {
      ltp_dec_[static_cast<std::size_t>(i)] =
          0.5f * (e_buf_[static_cast<std::size_t>(2 * i)] + e_buf_[static_cast<std::size_t>(2 * i + 1)]);
    }
    const int lo = std::max(2, cfg_.ltp_min_lag / 2);
    const int hi = std::min(half - 8, cfg_.ltp_max_lag / 2);
    int best_lag = 0;
    double best_score = 0.0;
    for (int lag = lo; lag <= hi; ++lag) {
      double num = 0.0, den = 0.0;
      for (int i = lag; i < half; ++i) {
        const double a = ltp_dec_[static_cast<std::size_t>(i)];
        const double b = ltp_dec_[static_cast<std::size_t>(i - lag)];
        num += a * b;
        den += b * b;
      }
      if (den <= 1e-12) continue;
      const double score = num / std::sqrt(den);
      if (score > best_score) {
        best_score = score;
        best_lag = lag;
      }
    }
    if (best_lag == 0) {
      ltp_gain_ *= 0.5f;
      return;
    }

    // Refine on the full-rate signal around the decimated estimate.
    int lag = 2 * best_lag;
    double best_corr = -1.0;
    int refined = lag;
    for (int cand = std::max(cfg_.ltp_min_lag, lag - 2); cand <= std::min(cfg_.ltp_max_lag, lag + 2); ++cand) {
      double num = 0.0, den = 0.0, energy = 0.0;
      for (int i = cand; i < n; ++i) {
        const double a = e_buf_[static_cast<std::size_t>(i)];
        const double b = e_buf_[static_cast<std::size_t>(i - cand)];
        num += a * b;
        den += b * b;
        energy += a * a;
      }
      if (den <= 1e-12 || energy <= 1e-12) continue;
      const double corr = num / std::sqrt(den * energy);
      if (corr > best_corr) {
        best_corr = corr;
        refined = cand;
        ltp_num_ = num;
        ltp_den_ = den;
      }
    }
    if (best_corr < static_cast<double>(cfg_.ltp_min_correlation)) {
      ltp_gain_ *= 0.5f;
      return;
    }
    ltp_lag_ = refined;
    const float gain = static_cast<float>(ltp_num_ / (ltp_den_ + 1e-12));
    ltp_gain_ = 0.5f * ltp_gain_ + 0.5f * std::max(0.0f, std::min(cfg_.ltp_max_gain, gain));
  }

  // Levinson-Durbin on the autocorrelation of the recent error signal.
  void updateArModel() {
    const int p = cfg_.ar_order;
    const int n = static_cast<int>(e_buf_.size());
    // Scratch buffers are members: this runs on the audio thread, so it must
    // not allocate.
    std::vector<double>& r = r_scratch_;
    std::fill(r.begin(), r.end(), 0.0);
    for (int lag = 0; lag <= p; ++lag) {
      double acc = 0.0;
      for (int i = lag; i < n; ++i) {
        acc += e_buf_[static_cast<std::size_t>(i)] * e_buf_[static_cast<std::size_t>(i - lag)];
      }
      r[static_cast<std::size_t>(lag)] = acc;
    }
    if (r[0] <= 1e-12) return;
    r[0] *= 1.0001;  // white-noise correction for numerical robustness
    const double r0 = r[0];

    std::vector<double>& a = a_scratch_;
    std::vector<double>& tmp = tmp_scratch_;
    std::fill(a.begin(), a.end(), 0.0);
    a[0] = 1.0;
    double err = r[0];
    for (int i = 1; i <= p; ++i) {
      double acc = r[static_cast<std::size_t>(i)];
      for (int j = 1; j < i; ++j) acc += a[static_cast<std::size_t>(j)] * r[static_cast<std::size_t>(i - j)];
      const double k = -acc / err;
      if (!std::isfinite(k) || std::abs(k) >= 1.0) return;  // keep the previous stable model
      std::copy(a.begin(), a.end(), tmp.begin());
      for (int j = 1; j < i; ++j) {
        a[static_cast<std::size_t>(j)] = tmp[static_cast<std::size_t>(j)] + k * tmp[static_cast<std::size_t>(i - j)];
      }
      a[static_cast<std::size_t>(i)] = k;
      err *= (1.0 - k * k);
      if (err <= 0.0) return;
    }
    // A whitening filter is only worth applying when the residual is actually
    // predictable. Fitting order-p noise to a white signal just injects a
    // time-varying filter into the gradient and wrecks the misadjustment.
    const double gain_db = 10.0 * std::log10(r0 / (err + 1e-30));
    if (gain_db < static_cast<double>(cfg_.ar_min_prediction_gain_db)) {
      a_[0] = 1.0f;
      for (std::size_t i = 1; i < a_.size(); ++i) a_[i] *= cfg_.ar_smoothing;
      return;
    }
    const float beta = cfg_.ar_smoothing;
    for (std::size_t i = 0; i < a_.size(); ++i) {
      a_[i] = beta * a_[i] + (1.0f - beta) * static_cast<float>(a[i]);
    }
    a_[0] = 1.0f;
  }

  AfcConfig cfg_;
  std::size_t n_;
  std::vector<float> h_, x_hist_, xw_hist_, a_, x_ar_, e_ar_, e_buf_;
  std::vector<double> r_scratch_, a_scratch_, tmp_scratch_;
  std::vector<float> x_ltp_, e_ltp_, ltp_dec_;
  std::size_t x_ltp_pos_ = 0, e_ltp_pos_ = 0;
  int ltp_lag_ = 0;
  float ltp_gain_ = 0.0f;
  double ltp_num_ = 0.0, ltp_den_ = 0.0;
  std::size_t pos_ = 0;
  static constexpr int kNormCheck = 64;
  static constexpr float kSubtractStep = 1.0f / 320.0f;  // 20 ms at 16 kHz
  static constexpr int kEnergyRefresh = 4096;
  double x_energy_ = 0.0, x_energy_avg_ = 0.0;
  int energy_refresh_ = 0;
  float y_env_ = 0.0f, e_env_ = 0.0f, d_env_ = 0.0f;
  float mu_now_ = 0.0f, mu_step_ = 1.0f, benefit_ = 1.0f, last_beta_ = 0.0f;
  float subtract_gate_ = 1.0f, subtract_target_ = 1.0f;
  double diag_y_power_ = 0.0, diag_e_power_ = 0.0, diag_beta_sum_ = 0.0;
  long diag_samples_ = 0;
  std::size_t first_tap_ = 0;
  AfcGate* gate_ = nullptr;
  bool fast_target_ = false;
  int norm_check_ = 0;
  std::size_t e_pos_ = 0;
  int since_ar_update_ = 0;
  int resets_ = 0;
  bool safety_frozen_ = false;
  bool adapting_ = false;
  int safety_bad_blocks_ = 0, safety_block_samples_ = 0, safety_freezes_ = 0;
  double safety_y_power_ = 0.0, safety_e_power_ = 0.0;
  double safety_last_block_erle_db_ = 0.0;
  float safety_norm_at_freeze_ = 0.0f;
};

}  // namespace ahs
