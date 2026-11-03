#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>
#include "fft.h"

namespace ahs {
// Experimental update-only PBFD Kalman. Audio stays in Afc's time-domain FIR.
// Exact project choices (not a verbatim paper implementation) are preregistered
// in docs/afc_rework_parameter_lock.md. No allocation in push()/update().
class PbfdKalman {
 public:
  static constexpr std::size_t L = 64, M = 128;
  PbfdKalman(std::size_t taps, std::size_t first, std::size_t order,
             std::size_t max_lag, double q_fraction = 2e-5)
      : n_(taps), first_(first), parts_((taps + L - 1) / L),
        history_(parts_ * L + M + order + max_lag + 4),
        x_(history_, 0), y_(history_, 0), white_((parts_ - 1) * L + M, 0),
        H_(parts_ * M), X_(parts_ * M), P_(parts_ * M, 0.016),
        post_(parts_ * M, 0.016), phi_(parts_ * M, 0),
        R_(M, 1e-12), E_(M), pred_(M), work_(M), fft_(M), q_fraction_(q_fraction) {}

  // Caller invokes after calculating this sample's output. An update at the
  // 64th sample can therefore only affect subsequent samples.
  bool push(float y, float x, const std::vector<float>& a, int lag, float b,
            std::vector<float>& h) {
    x_[pos_] = x; y_[pos_] = y;
    pos_ = (pos_ + 1) % history_;
    ++samples_;
    if (samples_ % L != 0) return false;
    update(a, lag, b, h);
    return true;
  }
  void reset() {
    std::fill(x_.begin(), x_.end(), 0); std::fill(y_.begin(), y_.end(), 0);
    std::fill(H_.begin(), H_.end(), C{}); std::fill(X_.begin(), X_.end(), C{});
    std::fill(P_.begin(), P_.end(), 0.016); std::fill(post_.begin(), post_.end(), 0.016);
    std::fill(phi_.begin(), phi_.end(), 0); std::fill(R_.begin(), R_.end(), 1e-12);
    confidence_ = 0; active_ = false; valid_ = true;
    // Absolute sample clock and ring index retained: host chunking must not
    // change update boundaries after a numerical reset.
  }
  double confidence() const { return confidence_; }
  bool valid() const { return valid_; }
  bool active() const { return active_; }
  std::size_t updates() const { return updates_; }
  std::size_t clips() const { return clips_; }
  const std::vector<double>& noisePsd() const { return R_; }
  const std::vector<double>& posterior() const { return post_; }
  // Read-only diagnostics for an independently computed rewhitening check.
  const std::vector<float>& whitenedReference() const { return white_; }

 private:
  using C = std::complex<float>;
  float raw(const std::vector<float>& v, std::size_t delay) const {
    return v[(pos_ + history_ - 1 - delay) % history_];
  }
  float whiten(const std::vector<float>& v, std::size_t delay,
               const std::vector<float>& a, int lag, float b) const {
    float short_now = 0, short_old = 0;
    for (std::size_t j = 0; j < a.size(); ++j) {
      short_now += a[j] * raw(v, delay + j);
      if (lag > 0 && b != 0)
        short_old += a[j] * raw(v, delay + j + static_cast<std::size_t>(lag));
    }
    return short_now - b * short_old;
  }
  void inverse(std::vector<C>& v) {
    for (auto& x : v) x = std::conj(x);
    fft_.forward(v.data());
    for (auto& x : v) x = std::conj(x) / static_cast<float>(M);
  }
  double bound(double v) {
    if (!std::isfinite(v)) { valid_ = false; return 0.016; }
    if (v > 1e6) { ++clips_; return 1e6; }
    return std::max(1e-14, v);
  }
  void update(const std::vector<float>& a, int lag, float b, std::vector<float>& h) {
    ++updates_;
    double sx = 0;
    for (std::size_t j = 0; j < L; ++j) sx += static_cast<double>(raw(x_, j)) * raw(x_, j);
    active_ = sx / L > 1e-12;
    if (!active_) {
      for (std::size_t j = 0; j < P_.size(); ++j)
        P_[j] = bound(P_[j] + std::max(1e-14, q_fraction_ * M * phi_[j]));
      return;
    }
    for (std::size_t j = 0; j < white_.size(); ++j) white_[j] = whiten(x_, j, a, lag, b);
    std::fill(pred_.begin(), pred_.end(), C{});
    for (std::size_t p = 0; p < parts_; ++p) {
      C* xx = X_.data() + p * M;
      for (std::size_t j = 0; j < M; ++j) xx[j] = C(white_[p * L + M - 1 - j], 0);
      fft_.forward(xx);
      for (std::size_t j = 0; j < M; ++j) pred_[j] += xx[j] * H_[p * M + j];
    }
    inverse(pred_);
    double sy = 0;
    std::fill(E_.begin(), E_.end(), C{});
    for (std::size_t j = 0; j < L; ++j) {
      const float yy = whiten(y_, L - 1 - j, a, lag, b);
      sy += static_cast<double>(yy) * yy;
      E_[L + j] = C(yy - pred_[L + j].real(), 0);
    }
    fft_.forward(E_.data());
    const double rf = std::max(1e-12, 1e-6 * sy / L);
    for (std::size_t j = 0; j < M; ++j) {
      double u = 0;
      for (std::size_t p = 0; p < parts_; ++p) {
        const std::size_t i = p * M + j;
        u += static_cast<double>(std::norm(X_[i])) * P_[i] / M;
      }
      const double inst = std::max(rf, static_cast<double>(std::norm(E_[j])) / L - u);
      R_[j] = std::max(rf, 0.91 * R_[j] + 0.09 * inst);
      if (!std::isfinite(R_[j]) || !std::isfinite(std::norm(E_[j]))) valid_ = false;
    }
    double uncertainty = 0, energy = 0;
    for (std::size_t p = 0; p < parts_; ++p) {
      double sum_post = 0;
      std::size_t valid_taps = 0;
      for (std::size_t j = 0; j < M; ++j) {
        const std::size_t i = p * M + j;
        const double x2 = std::norm(X_[i]);
        const double den = x2 * P_[i] + M * R_[j];
        const double scale = P_[i] / den;
        work_[j] = static_cast<float>(scale) * std::conj(X_[i]) * E_[j];
        post_[i] = bound((1.0 - (static_cast<double>(L) / M) * scale * x2) * P_[i]);
        sum_post += post_[i];
        // H still denotes the prior here. Update phi before writing new H.
        phi_[i] = 0.91 * phi_[i] + 0.09 * static_cast<double>(std::norm(H_[i])) / M;
        P_[i] = bound(post_[i] + std::max(1e-14, q_fraction_ * M * phi_[i]));
      }
      inverse(work_);
      for (std::size_t j = 0; j < L; ++j) {
        const std::size_t t = p * L + j;
        if (t < n_ && t >= first_) {
          h[t] += work_[j].real(); ++valid_taps;
          energy += static_cast<double>(h[t]) * h[t];
          if (!std::isfinite(h[t])) valid_ = false;
        } else if (t < n_) h[t] = 0;
      }
      uncertainty += static_cast<double>(valid_taps) / (M * M) * sum_post;
      for (std::size_t j = 0; j < M; ++j) {
        const std::size_t t = p * L + j;
        H_[p * M + j] = C(j < L && t < n_ ? h[t] : 0, 0);
      }
      fft_.forward(H_.data() + p * M);
    }
    confidence_ = energy <= 1e-20 ? 0 : std::clamp(1.0 - uncertainty / std::max(energy, 1e-20), 0.0, 1.0);
  }
  std::size_t n_, first_, parts_, history_, pos_ = 0, samples_ = 0, updates_ = 0, clips_ = 0;
  std::vector<float> x_, y_, white_;
  std::vector<C> H_, X_;
  std::vector<double> P_, post_, phi_, R_;
  std::vector<C> E_, pred_, work_;
  Fft fft_;
  double q_fraction_, confidence_ = 0;
  bool valid_ = true, active_ = false;
};
}  // namespace ahs
