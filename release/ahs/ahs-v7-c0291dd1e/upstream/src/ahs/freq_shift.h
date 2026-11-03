#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ahs {

// Single-sideband frequency shifter used as a decorrelator in the forward path.
//
// An AFC in a closed loop estimates its reference from a signal that already
// contains the target speech, so plain NLMS converges to a biased solution
// (PEM whitening only reduces that bias). Shifting the loudspeaker signal by a
// few Hz breaks the correlation between successive trips around the loop, which
// both removes most of that bias and raises the maximum stable gain by
// preventing the phase condition from being met at a fixed frequency
// (van Waterschoot & Moonen, "Fifty Years of Acoustic Feedback Control").
//
// A shift of ~5 Hz is inaudible for speech and mild for music.
// Cost: the FIR Hilbert transformer delays the signal by (taps-1)/2 samples.
class FrequencyShifter {
 public:
  FrequencyShifter(float shift_hz, float sample_rate, int taps = 65)
      : n_(static_cast<std::size_t>(taps | 1)),  // odd length: integer delay
        h_(n_, 0.0f),
        delay_(n_, 0.0f),
        sample_rate_(sample_rate),
        phase_inc_(2.0f * static_cast<float>(M_PI) * shift_hz / sample_rate),
        target_inc_(phase_inc_),
        // Ramp the rate over ~20 ms so switching it in is not a click.
        // Ramp rate for switching the shift in and out: full swing in ~20 ms.
        inc_step_(2.0f * static_cast<float>(M_PI) * 16.0f / (sample_rate * 0.02f * sample_rate)) {
    // Windowed ideal Hilbert transformer: h[k] = 2/(pi*k) for odd k, 0 for even.
    const int half = static_cast<int>(n_ / 2);
    for (int k = -half; k <= half; ++k) {
      const std::size_t i = static_cast<std::size_t>(k + half);
      if (k % 2 == 0) {
        h_[i] = 0.0f;
      } else {
        const float ideal = 2.0f / (static_cast<float>(M_PI) * static_cast<float>(k));
        // Hamming window keeps the passband ripple around 0.1 dB.
        const float w = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) *
                                                 static_cast<float>(i) /
                                                 static_cast<float>(n_ - 1));
        h_[i] = ideal * w;
      }
    }
  }

  // Group delay added to the signal path, in samples.
  int latencySamples() const { return static_cast<int>(n_ / 2); }

  // The shift rate can be changed while running: at 0 Hz the shifter degenerates
  // to the centre tap of its own delay line, i.e. a pure delay, so the engine
  // can leave it transparent until there is feedback to break up.
  void setShiftHz(float shift_hz) {
    target_inc_ = 2.0f * static_cast<float>(M_PI) * shift_hz / sample_rate_;
  }

  float shiftHz() const { return phase_inc_ * sample_rate_ / (2.0f * static_cast<float>(M_PI)); }

  float process(float x) {
    delay_[pos_] = x;
    pos_ = (pos_ + 1) % n_;

    // Quadrature component (oldest sample first, so the taps line up with h_).
    float q = 0.0f;
    for (std::size_t i = 0; i < n_; ++i) {
      std::size_t j = pos_ + i;
      if (j >= n_) j -= n_;
      q += h_[i] * delay_[j];
    }
    // In-phase component: the centre tap of the same delay line.
    std::size_t centre = pos_ + n_ / 2;
    if (centre >= n_) centre -= n_;
    const float in_phase = delay_[centre];

    const float out = in_phase * std::cos(phase_) - q * std::sin(phase_);
    phase_inc_ += std::max(-inc_step_, std::min(inc_step_, target_inc_ - phase_inc_));
    // Switching off means "stop shifting", and freezing at an arbitrary phase
    // would leave the signal permanently rotated between itself and its Hilbert
    // transform. So the oscillator keeps turning at a low rate until it comes
    // back to zero phase, where the shifter is exactly a delay line.
    if (target_inc_ == 0.0f && phase_ != 0.0f) {
      phase_inc_ = std::max(phase_inc_, kParkRate);
    }
    phase_ += phase_inc_;
    if (phase_ >= 2.0f * static_cast<float>(M_PI)) {
      phase_ -= 2.0f * static_cast<float>(M_PI);
      if (target_inc_ == 0.0f) {
        phase_ = 0.0f;
        phase_inc_ = 0.0f;
      }
    }
    return out;
  }

  void reset() {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    phase_ = 0.0f;
    pos_ = 0;
  }

 private:
  // 8 Hz at 16 kHz: the turn back to zero phase completes within ~125 ms, which
  // is short enough not to colour the metrics and still inaudible.
  static constexpr float kParkRate = 8.0f * 2.0f * 3.14159265358979f / 16000.0f;

  std::size_t n_;
  std::vector<float> h_, delay_;
  std::size_t pos_ = 0;
  float phase_ = 0.0f;
  float sample_rate_ = 16000.0f;
  float phase_inc_ = 0.0f;
  float target_inc_ = 0.0f;
  float inc_step_ = 1.0f;
};

}  // namespace ahs
