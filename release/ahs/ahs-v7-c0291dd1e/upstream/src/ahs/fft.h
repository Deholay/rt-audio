#pragma once
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace ahs {

// Minimal iterative radix-2 FFT. Sizes must be powers of two.
class Fft {
 public:
  explicit Fft(std::size_t n) : n_(n), twiddle_(n / 2), rev_(n) {
    if (n < 2 || (n & (n - 1)) != 0) throw std::invalid_argument("FFT size must be a power of two >= 2");
    scratch_.resize(n);  // magnitude() runs on the audio thread and must not allocate
    for (std::size_t k = 0; k < n / 2; ++k) {
      const double a = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n);
      twiddle_[k] = std::complex<float>(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < n) ++bits;
    for (std::size_t i = 0; i < n; ++i) {
      std::size_t r = 0;
      for (std::size_t b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
      rev_[i] = r;
    }
  }

  std::size_t size() const { return n_; }

  // In-place forward transform of a complex buffer of length n().
  void forward(std::complex<float>* data) const {
    for (std::size_t i = 0; i < n_; ++i) {
      if (i < rev_[i]) std::swap(data[i], data[rev_[i]]);
    }
    for (std::size_t len = 2; len <= n_; len <<= 1) {
      const std::size_t step = n_ / len;
      for (std::size_t i = 0; i < n_; i += len) {
        for (std::size_t j = 0; j < len / 2; ++j) {
          const std::complex<float> w = twiddle_[j * step];
          const std::complex<float> u = data[i + j];
          const std::complex<float> v = data[i + j + len / 2] * w;
          data[i + j] = u + v;
          data[i + j + len / 2] = u - v;
        }
      }
    }
  }

  // Real input of length n() -> magnitude spectrum of the first n()/2+1 bins.
  void magnitude(const float* real_in, float* mag_out) const {
    for (std::size_t i = 0; i < n_; ++i) scratch_[i] = std::complex<float>(real_in[i], 0.0f);
    forward(scratch_.data());
    for (std::size_t k = 0; k <= n_ / 2; ++k) mag_out[k] = std::abs(scratch_[k]);
  }

 private:
  std::size_t n_;
  std::vector<std::complex<float>> twiddle_;
  std::vector<std::size_t> rev_;
  mutable std::vector<std::complex<float>> scratch_;
};

}  // namespace ahs
