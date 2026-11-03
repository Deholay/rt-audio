#pragma once
#include <cmath>

namespace ahs {

// Transposed direct form II biquad. Sample-by-sample, zero algorithmic latency.
class Biquad {
 public:
  void setIdentity() {
    b0_ = 1.0f;
    b1_ = b2_ = a1_ = a2_ = 0.0f;
  }

  // RBJ parametric peaking filter with negative gain == a depth-controlled notch.
  // depth_db must be <= 0; q controls the notch width.
  void setNotch(float freq_hz, float sample_rate, float q, float depth_db) {
    const float a = std::pow(10.0f, depth_db / 40.0f);  // peaking gain, <1 for a cut
    const float w0 = 2.0f * static_cast<float>(M_PI) * freq_hz / sample_rate;
    const float cw = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * q);
    const float a0 = 1.0f + alpha / a;
    b0_ = (1.0f + alpha * a) / a0;
    b1_ = (-2.0f * cw) / a0;
    b2_ = (1.0f - alpha * a) / a0;
    a1_ = (-2.0f * cw) / a0;
    a2_ = (1.0f - alpha / a) / a0;
  }

  inline float process(float x) {
    const float y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return y;
  }

  void reset() { z1_ = z2_ = 0.0f; }

 private:
  float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
  float z1_ = 0.0f, z2_ = 0.0f;
};

}  // namespace ahs
