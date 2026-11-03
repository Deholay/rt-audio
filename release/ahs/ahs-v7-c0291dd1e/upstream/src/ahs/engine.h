#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "afc.h"
#include "detector.h"
#include "freq_shift.h"
#include "notch_bank.h"

namespace ahs {

struct EngineConfig {
  float sample_rate = 16000.0f;
  int block_size = 64;  // 4 ms @ 16 kHz -> detection decision rate
  bool enable_afc = true;
  bool enable_notch = true;
  bool enable_guard = true;  // fast gain-reduction safety net
  // Use the same AFC defaults as the simulator. The reference is the signal
  // at the loudspeaker, so forward I/O latency is not a lower bound on its
  // acoustic path to the microphone. Pin early taps only for a measured path.
  AfcConfig afc;
  DetectorConfig detector;
  NotchConfig notch;
  // Guard (broadband gain reduction) parameters.
  float guard_ratio_db = 6.0f;         // fast/slow envelope ratio that counts as a runaway
  float guard_attack_db_per_ms = 1.0f;
  float guard_release_db_per_s = 3.0f;
  float guard_max_reduction_db = 12.0f;
  // Forward-path decorrelation. Costs (hilbert_taps-1)/2 samples of latency.
  // Off in the delivered configuration. Measured benefit on the acceptance
  // matrix is 0.0 dB; measured cost on material that never howled is up to
  // 12 dB, and it is the only stage with an algorithmic delay (the Hilbert
  // FIR's 16 samples, i.e. 1 ms of the 10 ms budget). The stage is kept for
  // study and can be switched back on with `enable_shift`.
  bool enable_shift = false;
  float shift_hz = 8.0f;
  int hilbert_taps = 33;  // 16 samples of group delay at 16 kHz
  // How long the suppression stages stay switched in after the last detection.
  float engage_hold_s = 3.0f;
  // The shifter is the last resort: it is the only stage that alters the signal
  // even when it is working perfectly, so it is only switched in once howling
  // has survived the canceller and the notches for this long.
  float shift_engage_s = 0.5f;
  // ...and the tone must still be climbing at this rate while it is held down.
  float shift_growth_db_per_s = 30.0f;
  // Diagnostics only: keeps the shifter switched in unconditionally so that its
  // cost can be measured in an ablation.
  bool force_shift = false;
  // Decorrelation gate under study (item 2c): switch the shifter in whenever the
  // canceller is actually adapting. The canceller's bias under tonal, strongly
  // correlated sources is what pushes the narrowband loop gain past one, so the
  // decorrelation belongs where the adaptation is, not behind a detector that
  // only fires once the tone is already there. OR'd with the demand-driven rule
  // below, so it can only switch the shifter in earlier, never later.
  bool shift_when_afc_active = false;
  // The other gate under study: switch the shifter in once the detector has been
  // seeing howling continuously for `shift_engage_s`, without waiting for the
  // growth or filled-bank evidence the delivered path demands. Ored with the
  // conditions below, so it can only switch the shifter in earlier, never later.
  bool shift_when_howling = false;
  int shift_severe_notches = 6;  // above the bank size: guard-driven only
};

struct EngineStats {
  int active_notches = 0;
  float guard_reduction_db = 0.0f;
  bool howling_detected = false;  // detector flagged howling in the last frame
  std::vector<float> notch_freqs;
  std::vector<float> notch_depths_db;
  std::vector<float> notch_idle_s;
  // Every slot, active or not, in bank order, plus the running count of active
  // slots taken over by a different tone. Diagnostics for the notch bank.
  std::vector<float> slot_freqs_hz;
  std::vector<char> slot_active;
  long notch_steals = 0;
};

// Three-layer acoustic howling suppression:
//   1. AFC (PEM-NLMS)  - models and subtracts the feedback path; raises MSG
//                        without colouring the signal.
//   2. Detection+notch - kills the surviving critical frequencies; sample-rate
//                        biquads, so no added latency.
//   3. Guard           - fast broadband gain reduction as a last resort when the
//                        loop still runs away (e.g. strong nonlinear distortion).
class AhsEngine {
 public:
  explicit AhsEngine(const EngineConfig& cfg)
      : cfg_(cfg),
        afc_(cfg.afc, cfg.sample_rate),
        detector_([&] {
          DetectorConfig d = cfg.detector;
          d.frame_rate_hz = cfg.sample_rate / static_cast<float>(std::max(1, cfg.block_size));
          return d;
        }(),
                  cfg.sample_rate),
        notches_(cfg.notch, cfg.sample_rate),
        // Starts transparent: the engine switches the shift in on demand.
        shifter_(0.0f, cfg.sample_rate, cfg.hilbert_taps),
        frame_(static_cast<std::size_t>(cfg.detector.fft_size), 0.0f),
        linear_(static_cast<std::size_t>(cfg.detector.fft_size), 0.0f) {
    if (cfg_.block_size <= 0) throw std::invalid_argument("block_size must be positive");
    // Preallocate everything the audio thread needs: processBlock() must never
    // hit the allocator (malloc can block and cause priority inversion).
    freqs_.reserve(static_cast<std::size_t>(cfg.detector.max_candidates));
    growth_.reserve(static_cast<std::size_t>(cfg.detector.max_candidates));
    const float per_sample_db = cfg_.guard_attack_db_per_ms * 1000.0f / cfg_.sample_rate;
    attack_factor_ = std::pow(10.0f, -per_sample_db / 20.0f);
    release_factor_ = std::pow(10.0f, cfg_.guard_release_db_per_s / (cfg_.sample_rate * 20.0f));
    min_guard_gain_ = std::pow(10.0f, -cfg_.guard_max_reduction_db / 20.0f);
  }

  // mic: microphone samples of this block.
  // loudspeaker: samples that were actually played during this block.
  // out: processed block (same length).
  void processBlock(const float* mic, const float* loudspeaker, float* out, int n) {
    for (int i = 0; i < n; ++i) {
      // A single NaN from a driver or an upstream plugin would otherwise poison
      // the adaptive filter state forever.
      float s = std::isfinite(mic[i]) ? mic[i] : 0.0f;
      const float x = std::isfinite(loudspeaker[i]) ? loudspeaker[i] : 0.0f;
      if (cfg_.enable_afc) s = afc_.process(s, x);

      // Sliding analysis frame is fed with the AFC output.
      frame_[frame_pos_] = s;
      frame_pos_ = (frame_pos_ + 1) % frame_.size();

      if (cfg_.enable_notch) s = notches_.process(s);
      // Level envelopes are tracked whether or not the guard is enabled: the
      // AFC's fast/slow switch uses them too.
      const float as = std::abs(s);
      fast_env_ = 0.99f * fast_env_ + 0.01f * as;      // ~6 ms @ 16 kHz
      slow_env_ = 0.9995f * slow_env_ + 0.0005f * as;  // ~125 ms
      if (cfg_.enable_guard) s = applyGuard(s);
      // Last stage: what leaves here is what reaches the loudspeaker, so the
      // shift applies to the whole loop.
      if (cfg_.enable_shift) s = shifter_.process(s);
      out[i] = s;
      // Host calls may be partial or span several hops. Count samples rather
      // than calls so persistence, growth and suppression clocks stay aligned.
      if (++samples_since_detect_ == cfg_.block_size) {
        samples_since_detect_ = 0;
        runDetection(static_cast<float>(cfg_.block_size) / cfg_.sample_rate);
      }
    }
  }

  // Real-time safe: the detector's verdict for the most recent frame.
  bool howlingDetected() const { return howling_now_; }

  // Substitutes an external decision rule for the canceller's built-in benefit
  // gate. Nullptr (the default) leaves the delivered behaviour untouched; the
  // gate is owned by the caller. Used by the offline gate sweep.
  void setAfcGate(AfcGate* gate) { afc_.setGate(gate); }

  // Offline diagnostics.
  double afcErleDb() const { return afc_.diagErleDb(); }
  double afcMeanBeta() const { return afc_.diagMeanBeta(); }
  const std::vector<float>& afcCoefficients() const { return afc_.coefficients(); }
  float afcGateGain() const { return afc_.diagGateGain(); }
  float afcLastBeta() const { return afc_.diagLastBeta(); }
  float afcErrEnv() const { return afc_.diagErrEnv(); }
  float afcMicEnv() const { return afc_.diagMicEnv(); }
  float afcEstEnv() const { return afc_.diagEstEnv(); }
  bool afcSafetyFrozen() const { return afc_.safetyFrozen(); }
  int afcSafetyFreezes() const { return afc_.safetyFreezeCount(); }
  double afcSafetyBlockErleDb() const { return afc_.safetyLastBlockErleDb(); }

  // Not real-time safe (allocates); call it from the control thread.
  EngineStats stats() const {
    EngineStats st;
    st.active_notches = notches_.activeCount();
    st.howling_detected = howling_now_;
    st.guard_reduction_db = 20.0f * std::log10(guard_gain_ + 1e-20f);
    for (const auto& s : notches_.states()) {
      if (!s.active) continue;
      st.notch_freqs.push_back(s.freq_hz);
      st.notch_depths_db.push_back(s.depth_db);
      st.notch_idle_s.push_back(s.idle_seconds);
    }
    for (const auto& s : notches_.states()) {
      st.slot_freqs_hz.push_back(s.freq_hz);
      st.slot_active.push_back(s.active ? 1 : 0);
    }
    st.notch_steals = notches_.stealCount();
    return st;
  }

  // Algorithmic latency of the processing path in samples (excluding the host's
  // I/O buffering). AFC, notches and guard are sample-based and add none; only
  // the optional frequency shifter's Hilbert FIR does.
  int algorithmicLatencySamples() const {
    return cfg_.enable_shift ? shifter_.latencySamples() : 0;
  }

  // True while the suppression stages are switched in (offline diagnostics).
  bool suppressionEngaged() const { return engage_hold_s_ > 0.0f; }
  bool shiftEngaged() const { return shift_engaged_; }
  const HowlingDetector& detector() const { return detector_; }

  // The most recent `n` samples of the signal the detector analyses (the AFC
  // output), oldest first, for offline analysers that need the same input at a
  // different time-frequency resolution. Reads state, changes none.
  void analysisTail(float* dst, int n) const {
    const int have = static_cast<int>(frame_.size());
    for (int i = 0; i < n; ++i) {
      const int back = n - i;
      dst[i] = back > have
                   ? 0.0f
                   : frame_[(frame_pos_ + frame_.size() - static_cast<std::size_t>(back)) %
                            frame_.size()];
    }
  }

 private:
  void runDetection(float elapsed_seconds) {
    // Unwrap the circular analysis buffer into chronological order.
    for (std::size_t i = 0; i < frame_.size(); ++i) {
      linear_[i] = frame_[(frame_pos_ + i) % frame_.size()];
    }
    const auto& cands = detector_.analyze(linear_.data());
    freqs_.clear();
    growth_.clear();
    for (const auto& c : cands) {
      freqs_.push_back(c.freq_hz);
      growth_.push_back(c.growth_db_per_s);
    }
    notches_.update(freqs_, growth_, elapsed_seconds);
    howling_now_ = !freqs_.empty();
    // Fast adaptation costs output SNR (NLMS misadjustment lands in the signal),
    // so it is only bought while the detector actually sees howling. Level-based
    // triggers were tried and fire on every speech onset.
    afc_.setFastAdapt(howling_now_);

    // Suppression is demand driven. Cancelling and shifting cost output quality
    // (adaptation residual, Hilbert sideband, notch depth), and in a loop that
    // is nowhere near instability that cost buys nothing, so both stages stay
    // transparent until there is evidence of feedback and are released again
    // after a hold time. The canceller keeps adapting throughout, so it is
    // already converged when it is switched in.
    // The evidence is the detector, not the canceller's own estimate: a biased
    // filter reports feedback that is not there, which would leave the engine
    // permanently engaged and defeat the point.
    const bool evidence = howling_now_ || notches_.activeCount() > 0;
    if (evidence) {
      engage_hold_s_ = cfg_.engage_hold_s;
    } else {
      engage_hold_s_ = std::max(0.0f, engage_hold_s_ - elapsed_seconds);
    }
    const bool engaged = engage_hold_s_ > 0.0f;
    afc_.setSubtractEnabled(engaged);

    // Time the detector has *continuously* been seeing howling. It decays again
    // when it is not, because the shifter's trigger has to mean "this tone is
    // surviving the earlier stages" -- accumulating it over the whole run let
    // scattered false detections add up to the trigger and left the shifter,
    // whose measured benefit is 0.0 dB, permanently switched into clean speech.
    if (howling_now_) {
      howling_time_s_ += elapsed_seconds;
    } else {
      howling_time_s_ = std::max(0.0f, howling_time_s_ - elapsed_seconds);
    }
    // The shifter is the only stage that alters every sample it sees, so it has
    // to mean "the notches are not winning". Two independent signs of that: the
    // detected tone is still climbing despite them, or the bank has filled up
    // while the detector still sees howling. A dip of the broadband guard is
    // not one (it dips on ordinary transients) and neither is elapsed time on
    // its own -- both switched it into material that was never howling.
    float growth_max = 0.0f;
    for (float g : growth_) growth_max = std::max(growth_max, g);
    const bool severe =
        howling_now_ && notches_.activeCount() >= cfg_.shift_severe_notches;
    const bool losing = howling_time_s_ >= cfg_.shift_engage_s &&
                        growth_max >= cfg_.shift_growth_db_per_s;
    const bool afc_active =
        cfg_.shift_when_afc_active && cfg_.enable_afc && afc_.adaptingNow();
    // `howling_time_s_ > 0` is what makes this a gate rather than always-on:
    // with an engage time of zero the shifter still waits for the detector's
    // first howling block, and follows the same decay when it stops.
    const bool howl_gate = cfg_.shift_when_howling && howling_time_s_ > 0.0f &&
                           howling_time_s_ >= cfg_.shift_engage_s;
    shift_engaged_ = cfg_.force_shift || afc_active || howl_gate ||
                     (engaged && (severe || losing));
    if (cfg_.enable_shift) shifter_.setShiftHz(shift_engaged_ ? cfg_.shift_hz : 0.0f);
  }

  // Ramps a linear gain multiplicatively, which is the same exponential ramp as
  // stepping in dB but without a pow() per sample.
  float applyGuard(float x) {
    const bool runaway = fast_env_ > ratio_linear_ * (slow_env_ + 1e-9f);

    if (howling_now_ && runaway) {
      guard_gain_ = std::max(min_guard_gain_, guard_gain_ * attack_factor_);
    } else {
      guard_gain_ = std::min(1.0f, guard_gain_ * release_factor_);
    }
    return x * guard_gain_;
  }

  EngineConfig cfg_;
  Afc afc_;
  HowlingDetector detector_;
  NotchBank notches_;
  FrequencyShifter shifter_;
  std::vector<float> frame_, linear_, freqs_, growth_;
  float guard_gain_ = 1.0f;
  float attack_factor_ = 1.0f, release_factor_ = 1.0f, min_guard_gain_ = 1.0f;
  const float ratio_linear_ = std::pow(10.0f, cfg_.guard_ratio_db / 20.0f);
  std::size_t frame_pos_ = 0;
  int samples_since_detect_ = 0;
  bool howling_now_ = false;
  float fast_env_ = 0.0f, slow_env_ = 0.0f;
  float engage_hold_s_ = 0.0f;
  float howling_time_s_ = 0.0f;
  bool shift_engaged_ = false;
};

}  // namespace ahs
