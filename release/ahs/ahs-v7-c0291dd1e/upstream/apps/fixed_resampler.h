#pragma once
#include "speex_resampler.h"
#include <cstdint>
#include <stdexcept>

namespace ahs_live {
// Allocation is confined to construction. The upstream state preserves phase
// across packets; callers must honor BOTH returned counts on every call.
class FixedResampler {
 public:
  static constexpr int quality = 4;
  FixedResampler(unsigned input_rate, unsigned output_rate) {
    if (!((input_rate == 48000 && output_rate == 16000) ||
          (input_rate == 16000 && output_rate == 48000)))
      throw std::invalid_argument("resampler requires 48000 <-> 16000 Hz");
    int error = RESAMPLER_ERR_SUCCESS;
    state_ = speex_resampler_init(1, input_rate, output_rate, quality, &error);
    if (!state_ || error != RESAMPLER_ERR_SUCCESS) {
      if (state_) speex_resampler_destroy(state_);
      throw std::runtime_error("Speex Q4 initialization failed");
    }
  }
  ~FixedResampler() { speex_resampler_destroy(state_); }
  FixedResampler(const FixedResampler&) = delete;
  FixedResampler& operator=(const FixedResampler&) = delete;
  int process(const float* input, spx_uint32_t& input_count,
              float* output, spx_uint32_t& output_count) noexcept {
    return speex_resampler_process_float(state_, 0, input, &input_count,
                                         output, &output_count);
  }
  int reset() noexcept { return speex_resampler_reset_mem(state_); }
  int input_latency() const noexcept {
    return speex_resampler_get_input_latency(state_);
  }
  int output_latency() const noexcept {
    return speex_resampler_get_output_latency(state_);
  }
 private:
  SpeexResamplerState* state_ = nullptr;
};
}  // namespace ahs_live
