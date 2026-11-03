#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "fft.h"

namespace ahs {

struct DetectorConfig {
  int fft_size = 256;       // 16 ms @ 16 kHz
  // Rate at which analyze() is called; only used to express growth in dB/s.
  float frame_rate_hz = 250.0f;
  int history_frames = 50;  // Q_M for IPMP / IMSD; 200 ms at a 4 ms hop
  float papr_threshold_db = 15.0f;
  float phpr_threshold_db = 15.0f;
  // Harmonic *and* sub-harmonic ratios: 0.5 is what rejects an upper harmonic of
  // voiced speech being mistaken for a howling tone.
  std::vector<float> phpr_harmonics = {0.5f, 1.5f, 2.0f, 3.0f};
  float ipmp_threshold = 0.8f;               // fraction of recent frames the peak persisted
  float imsd_threshold_db = 1.0f;            // max deviation from a linear dB ramp
  // Fast path: a strongly protruding, rapidly growing tone is acted on after
  // `fast_frames` instead of waiting for the full persistence window.
  int fast_frames = 5;
  float fast_papr_threshold_db = 20.0f;
  float fast_ipmp_threshold = 0.9f;
  // Growth path: a tone that is climbing along a straight line in dB is a loop
  // ringing up, and it can be caught long before it is prominent enough for the
  // fast path. A steady tone (music, a held vowel, a test burst) has no slope,
  // which is what keeps this from firing on programme material.
  int growth_frames = 25;              // 100 ms at a 4 ms hop
  float growth_db_per_frame = 0.5f;    // 12.5 dB over that window
  float growth_max_deviation_db = 3.0f;
  // Left at the main PAPR threshold: 12 dB buys the growth path 12-15 dB
  // candidates, and across the v9 matrix that bought one extra invariant
  // violation and a higher worst peak with no detection gain. Lower it
  // deliberately (ahs_sim --growth-papr-db) when experimenting.
  float growth_papr_threshold_db = 15.0f;
  // A howling tone is as narrow as the analysis window allows; a harmonic of a
  // voiced sound sits on a formant and is correspondingly broad. Requiring the
  // skirts two bins away to be well down is what keeps the growth path off
  // rising speech and music.
  float growth_sharpness_db = 12.0f;
  // A howling tone sits at a pole of the loop and stays there; the harmonics of
  // a voiced sound ride on a pitch contour that never holds still for long.
  // Measured over `stability_frames`, that separates the two far better than the
  // +/-1 bin tolerance of the persistence criterion (a bin is 62.5 Hz here).
  int stability_frames = 8;             // 48 ms at a 4 ms hop
  float max_freq_deviation_hz = 6.0f;
  // Same test over the whole persistence window (history_frames).
  float max_persistent_deviation_hz = 16.0f;
  float min_freq_hz = 100.0f;
  float max_freq_hz = 7000.0f;
  int max_candidates = 8;
  // Window for the temporal sparsity feature reported by observe(). Zero means
  // history_frames. Feature reporting only: no decision reads it.
  int ninos_frames = 0;
};

struct HowlingCandidate {
  int bin = 0;
  float freq_hz = 0.0f;
  float papr_db = 0.0f;
  float phpr_db = 0.0f;
  float ipmp = 0.0f;
  float imsd_db = 0.0f;
  // How fast this component is climbing, in dB per second, from a linear fit
  // over the growth window. A loop that is running away climbs; a tonal part of
  // programme material does not, which is what makes this usable as the
  // urgency of the suppression rather than only as a detection feature.
  float growth_db_per_s = 0.0f;
  float score = 0.0f;  // higher == more confident
};

// Everything the detector can say about one spectral peak in the current frame,
// with no threshold applied. Produced by observe() for offline evaluation of the
// features themselves; the detection decision does not use this struct.
struct DetectorFeatures {
  int bin = 0;
  float freq_hz = 0.0f;
  float level_db = 0.0f;      // peak level, 10*log10 of the bin power
  float mean_db = 0.0f;       // frame average, the PAPR reference
  // Median level of the bins around the peak, skipping its own skirts: the
  // local noise floor this peak would have to rise above to be visible at all.
  float noise_floor_db = 0.0f;
  float papr_db = 0.0f;
  float phpr_db = 0.0f;
  float ipmp = 0.0f;          // over history_frames
  float ipmp_fast = 0.0f;     // over fast_frames
  float imsd_db = 0.0f;       // -1 when the bin is not growing
  float growth_db_per_s = 0.0f;
  float sharpness_db = 0.0f;
  float freq_dev_hz = 0.0f;           // over stability_frames
  float freq_dev_persist_hz = 0.0f;   // over history_frames
  // NINOS^2-T: temporal sparsity of this single bin, l2/l4 norm ratio over the
  // sparsity window, normalised to (1/sqrt(M), 1]. 1.0 means the bin held the
  // same power for the whole window; small values mean its energy sat in a few
  // frames. A howling tone is temporally dense, a speech harmonic is not.
  // Feature only: nothing in the detection decision reads it.
  float ninos_t = 0.0f;
  // True when analyze() put this peak on its candidate list in this frame, i.e.
  // when the delivered decision logic fired on it.
  bool selected = false;
};

// Frame-based howling detector implementing the criteria compared in
// van Waterschoot & Moonen, "Comparative Evaluation of Howling Detection
// Criteria in Notch-Filter-Based Howling Suppression", JAES 58(11), 2010.
class HowlingDetector {
 public:
  HowlingDetector(const DetectorConfig& cfg, float sample_rate)
      : cfg_(cfg),
        sample_rate_(sample_rate),
        frame_rate_(cfg.frame_rate_hz > 0.0f ? cfg.frame_rate_hz : 1.0f),
        fft_(static_cast<std::size_t>(cfg.fft_size)),
        window_(static_cast<std::size_t>(cfg.fft_size)),
        frame_(static_cast<std::size_t>(cfg.fft_size), 0.0f),
        windowed_(static_cast<std::size_t>(cfg.fft_size), 0.0f),
        mag_(static_cast<std::size_t>(cfg.fft_size / 2 + 1), 0.0f),
        logmag_(static_cast<std::size_t>(cfg.fft_size / 2 + 1), -120.0f) {
    // Everything the audio thread touches is allocated here, once: analyze()
    // must not call into the allocator.
    if (cfg_.history_frames < 4) cfg_.history_frames = 4;
    history_.resize(static_cast<std::size_t>(cfg_.history_frames));
    for (Frame& f : history_) f.logmag.assign(mag_.size(), -120.0f);
    candidates_.reserve(mag_.size());
    for (std::size_t i = 0; i < window_.size(); ++i) {
      window_[i] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * static_cast<float>(i) /
                                          static_cast<float>(window_.size()));
    }
  }

  // `frame` must hold fft_size samples (most recent sample last).
  const std::vector<HowlingCandidate>& analyze(const float* frame) {
    const std::size_t n = static_cast<std::size_t>(cfg_.fft_size);
    for (std::size_t i = 0; i < n; ++i) windowed_[i] = frame[i] * window_[i];
    fft_.magnitude(windowed_.data(), mag_.data());

    const std::size_t nbins = mag_.size();
    double mean_power = 0.0;
    for (std::size_t k = 0; k < nbins; ++k) {
      const float p = mag_[k] * mag_[k] + 1e-20f;
      mean_power += p;
      logmag_[k] = 10.0f * std::log10(p);
    }
    mean_power /= static_cast<double>(nbins);
    const float mean_db = 10.0f * std::log10(static_cast<float>(mean_power) + 1e-20f);

    // Overwrite the oldest slot in the preallocated ring.
    Frame& slot = history_[head_];
    std::copy(logmag_.begin(), logmag_.end(), slot.logmag.begin());
    slot.mean_db = mean_db;
    head_ = (head_ + 1) % history_.size();
    if (filled_ < history_.size()) ++filled_;

    const float bin_hz = sample_rate_ / static_cast<float>(cfg_.fft_size);
    const int k_min = std::max(1, static_cast<int>(cfg_.min_freq_hz / bin_hz));
    const int k_max = std::min(static_cast<int>(nbins) - 2, static_cast<int>(cfg_.max_freq_hz / bin_hz));

    candidates_.clear();
    for (int k = k_min; k <= k_max; ++k) {
      const std::size_t uk = static_cast<std::size_t>(k);
      if (logmag_[uk] < logmag_[uk - 1] || logmag_[uk] < logmag_[uk + 1]) continue;  // local peak only
      const float papr = logmag_[uk] - mean_db;
      if (papr < std::min(cfg_.papr_threshold_db, cfg_.growth_papr_threshold_db)) continue;

      HowlingCandidate c;
      c.bin = k;
      c.papr_db = papr;
      c.phpr_db = phpr(k);
      c.ipmp = ipmp(k, static_cast<int>(filled_));
      c.imsd_db = imsd(k);
      c.freq_hz = refineFrequency(k);
      c.growth_db_per_s = slopeDbPerFrame(k, cfg_.growth_frames) * frame_rate_;

      // Persistence criteria are meaningless until enough frames have been
      // seen: with a half-filled history every peak looks like it has been
      // there forever, which fired on the first syllable after a restart.
      const int seen = static_cast<int>(filled_);
      const bool phpr_ok = c.phpr_db >= cfg_.phpr_threshold_db;
      // The persistence path claims the peak has been in the same place for the
      // whole history window, so the frequency has to hold still for that whole
      // window too. Checking stability only over the short window let a held
      // vowel qualify: its harmonics persist, but they wander, and that is what
      // was putting notches into clean speech.
      const bool ipmp_ok = c.papr_db >= cfg_.papr_threshold_db &&
                           seen >= cfg_.history_frames && c.ipmp >= cfg_.ipmp_threshold &&
                           freqDeviationHz(k, cfg_.history_frames) <= cfg_.max_persistent_deviation_hz;
      const bool stable = seen >= cfg_.stability_frames && freqDeviationHz(k) <= cfg_.max_freq_deviation_hz;
      const bool imsd_ok = c.imsd_db >= 0.0f && c.imsd_db <= cfg_.imsd_threshold_db;
      // PHPR is the strongest single feature (largest AUC in the JAES study) but
      // on its own it fires on speech harmonics; pairing it with the persistence
      // criterion is what keeps the false-alarm rate usable. IMSD only adds
      // confidence to the ranking.
      const bool fast_ok = c.papr_db >= cfg_.papr_threshold_db &&
                           seen >= cfg_.fast_frames && c.papr_db >= cfg_.fast_papr_threshold_db && imsd_ok &&
                           ipmp(k, cfg_.fast_frames) >= cfg_.fast_ipmp_threshold;
      const bool growth_ok =
          seen >= cfg_.growth_frames && c.papr_db >= cfg_.growth_papr_threshold_db && sharpness(k) >= cfg_.growth_sharpness_db && growing(k);
      if (!phpr_ok || !stable || !(ipmp_ok || fast_ok || growth_ok)) continue;

      if (fast_ok) ++fired_fast_;
      if (growth_ok) ++fired_growth_;
      if (ipmp_ok) ++fired_ipmp_;
      c.score = c.papr_db + c.phpr_db + 20.0f * c.ipmp;
      candidates_.push_back(c);
    }

    std::sort(candidates_.begin(), candidates_.end(),
              [](const HowlingCandidate& a, const HowlingCandidate& b) { return a.score > b.score; });
    if (static_cast<int>(candidates_.size()) > cfg_.max_candidates) {
      candidates_.resize(static_cast<std::size_t>(cfg_.max_candidates));
    }
    return candidates_;
  }

  const std::vector<float>& lastLogMagnitude() const { return logmag_; }

  // Every local peak of the current frame with all features evaluated and no
  // threshold applied, strongest first, at most `max_peaks` of them (<= 0 for
  // all). Reads the state analyze() left behind and changes none of it, so the
  // detection path is unaffected by whether this is called.
  //
  // NOT real-time safe: it allocates (through `out`) and runs a pow() per frame
  // of history per peak. Offline evaluation only.
  void observe(std::vector<DetectorFeatures>& out, int max_peaks = 0) const {
    out.clear();
    if (filled_ == 0) return;
    const std::size_t nbins = logmag_.size();
    const float bin_hz = sample_rate_ / static_cast<float>(cfg_.fft_size);
    const int k_min = std::max(1, static_cast<int>(cfg_.min_freq_hz / bin_hz));
    const int k_max = std::min(static_cast<int>(nbins) - 2,
                               static_cast<int>(cfg_.max_freq_hz / bin_hz));
    const float mean_db = recent(1, 0).mean_db;
    const int ninos_frames =
        cfg_.ninos_frames > 0 ? cfg_.ninos_frames : cfg_.history_frames;

    for (int k = k_min; k <= k_max; ++k) {
      const std::size_t uk = static_cast<std::size_t>(k);
      if (logmag_[uk] < logmag_[uk - 1] || logmag_[uk] < logmag_[uk + 1]) continue;
      DetectorFeatures f;
      f.bin = k;
      f.freq_hz = refineFrequency(k);
      f.level_db = logmag_[uk];
      f.mean_db = mean_db;
      f.papr_db = logmag_[uk] - mean_db;
      f.noise_floor_db = localFloorDb(k);
      f.phpr_db = phpr(k);
      f.ipmp = ipmp(k, cfg_.history_frames);
      f.ipmp_fast = ipmp(k, cfg_.fast_frames);
      f.imsd_db = imsd(k);
      f.growth_db_per_s = slopeDbPerFrame(k, cfg_.growth_frames) * frame_rate_;
      f.sharpness_db = sharpness(k);
      f.freq_dev_hz = freqDeviationHz(k, cfg_.stability_frames);
      f.freq_dev_persist_hz = freqDeviationHz(k, cfg_.history_frames);
      f.ninos_t = ninosT(k, ninos_frames);
      for (const HowlingCandidate& c : candidates_) {
        if (c.bin == k) {
          f.selected = true;
          break;
        }
      }
      out.push_back(f);
    }

    std::sort(out.begin(), out.end(), [](const DetectorFeatures& a, const DetectorFeatures& b) {
      return a.papr_db > b.papr_db;
    });
    if (max_peaks > 0 && static_cast<int>(out.size()) > max_peaks) {
      out.resize(static_cast<std::size_t>(max_peaks));
    }
  }

  // Offline diagnostics: how often each criterion fired.
  long firedFast() const { return fired_fast_; }
  long firedGrowth() const { return fired_growth_; }
  long firedIpmp() const { return fired_ipmp_; }

 private:
  struct Frame {
    std::vector<float> logmag;
    float mean_db = -120.0f;
  };

  // Peak-to-harmonic power ratio: howling is an isolated tone, so the power at
  // its (sub)harmonic positions is just background. A tonal component of voiced
  // speech or music, in contrast, sits in a harmonic series.
  float phpr(int k) const {
    float worst = 1e9f;
    const float nbins = static_cast<float>(logmag_.size());
    for (float m : cfg_.phpr_harmonics) {
      const float kh = static_cast<float>(k) * m;
      if (kh < 1.0f || kh >= nbins - 1.0f) continue;
      worst = std::min(worst, logmag_[static_cast<std::size_t>(k)] - interpLogMag(kh));
    }
    return worst == 1e9f ? 0.0f : worst;
  }

  float interpLogMag(float bin) const {
    const int i = static_cast<int>(bin);
    const float t = bin - static_cast<float>(i);
    return (1.0f - t) * logmag_[static_cast<std::size_t>(i)] + t * logmag_[static_cast<std::size_t>(i + 1)];
  }

  // Chronological access into the ring: age 0 == oldest of the `q` most recent.
  const Frame& recent(int q, int age) const {
    const std::size_t idx =
        (head_ + history_.size() - static_cast<std::size_t>(q) + static_cast<std::size_t>(age)) % history_.size();
    return history_[idx];
  }

  // Interframe peak magnitude persistence: fraction of recent frames in which
  // this bin (+/- 1) was a local maximum *and* stood out from the frame average.
  float ipmp(int k, int frames) const {
    const int q = std::min(frames, static_cast<int>(filled_));
    if (q <= 0) return 0.0f;
    int hits = 0;
    for (int age = 0; age < q; ++age) {
      const Frame& f = recent(q, age);
      const std::vector<float>& h = f.logmag;
      const int lo = std::max(1, k - 1);
      const int hi = std::min(static_cast<int>(h.size()) - 2, k + 1);
      for (int j = lo; j <= hi; ++j) {
        if (h[static_cast<std::size_t>(j)] >= h[static_cast<std::size_t>(j - 1)] &&
            h[static_cast<std::size_t>(j)] >= h[static_cast<std::size_t>(j + 1)] &&
            h[static_cast<std::size_t>(j)] - f.mean_db >= cfg_.papr_threshold_db) {
          ++hits;
          break;
        }
      }
    }
    return static_cast<float>(hits) / static_cast<float>(q);
  }

  // Interframe magnitude slope deviation: a howling component grows along a
  // straight line in dB until saturation, so the residual of a linear fit is small.
  // Returns -1 when there is no positive growth (i.e. criterion not satisfied).
  float imsd(int k) const {
    const int q = std::min(cfg_.fast_frames, static_cast<int>(filled_));
    if (q < 4) return -1.0f;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < q; ++i) {
      const double x = i;
      const double y = recent(q, i).logmag[static_cast<std::size_t>(k)];
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
    }
    const double denom = q * sxx - sx * sx;
    if (std::abs(denom) < 1e-12) return -1.0f;
    const double slope = (q * sxy - sx * sy) / denom;
    const double intercept = (sy - slope * sx) / q;
    if (slope <= 0.0) return -1.0f;  // not growing => not howling
    double dev = 0.0;
    for (int i = 0; i < q; ++i) {
      const double pred = intercept + slope * i;
      dev += std::abs(recent(q, i).logmag[static_cast<std::size_t>(k)] - pred);
    }
    return static_cast<float>(dev / q);
  }

  // NINOS^2-T: l2/l4 norm ratio of bin `k` along the time axis, over the last
  // `frames` frames. sum(a^2) / (sqrt(M) * sqrt(sum(a^4))) with a the magnitude,
  // which is 1.0 for a bin whose power never changed and 1/sqrt(M) for one that
  // had all its energy in a single frame. The normalisation is written out here
  // so it can be changed without guessing what the number means.
  float ninosT(int k, int frames) const {
    const int q = std::min(frames, static_cast<int>(filled_));
    if (q < 4) return 0.0f;
    double s2 = 0.0, s4 = 0.0;
    for (int age = 0; age < q; ++age) {
      // logmag holds 10*log10(power), so this is a^2 for magnitude a.
      const double p = std::pow(10.0, static_cast<double>(recent(q, age).logmag[static_cast<std::size_t>(k)]) / 10.0);
      s2 += p;
      s4 += p * p;
    }
    return static_cast<float>(s2 / (std::sqrt(static_cast<double>(q)) * std::sqrt(s4) + 1e-30));
  }

  // Median level of the neighbourhood of bin `k`, excluding the peak's own
  // skirts (+/- 3 bins). Offline use only: it copies and sorts.
  float localFloorDb(int k) const {
    const int n = static_cast<int>(logmag_.size());
    const int span = 32, skip = 3;
    std::vector<float> around;
    around.reserve(static_cast<std::size_t>(2 * span));
    for (int j = std::max(0, k - span); j <= std::min(n - 1, k + span); ++j) {
      if (std::abs(j - k) <= skip) continue;
      around.push_back(logmag_[static_cast<std::size_t>(j)]);
    }
    if (around.empty()) return recent(1, 0).mean_db;
    const std::size_t mid = around.size() / 2;
    std::nth_element(around.begin(), around.begin() + static_cast<std::ptrdiff_t>(mid),
                     around.end());
    return around[mid];
  }

  // Level of the peak above its skirts two bins out (both sides).
  float sharpness(int k) const {
    const int n = static_cast<int>(logmag_.size());
    const int lo = std::max(0, k - 2), hi = std::min(n - 1, k + 2);
    return logmag_[static_cast<std::size_t>(k)] -
           std::max(logmag_[static_cast<std::size_t>(lo)], logmag_[static_cast<std::size_t>(hi)]);
  }

  // Least-squares slope of bin `k` in dB per frame over the last `frames`.
  float slopeDbPerFrame(int k, int frames) const {
    const int q = std::min(frames, static_cast<int>(filled_));
    if (q < 4) return 0.0f;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < q; ++i) {
      const double x = i;
      const double y = recent(q, i).logmag[static_cast<std::size_t>(k)];
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
    }
    const double denom = q * sxx - sx * sx;
    if (std::abs(denom) < 1e-12) return 0.0f;
    return static_cast<float>((q * sxy - sx * sy) / denom);
  }

  // True when bin `k` has been climbing steadily in dB over the growth window.
  bool growing(int k) const {
    const int q = std::min(cfg_.growth_frames, static_cast<int>(filled_));
    if (q < 8) return false;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < q; ++i) {
      const double x = i;
      const double y = recent(q, i).logmag[static_cast<std::size_t>(k)];
      sx += x;
      sy += y;
      sxx += x * x;
      sxy += x * y;
    }
    const double denom = q * sxx - sx * sx;
    if (std::abs(denom) < 1e-12) return false;
    const double slope = (q * sxy - sx * sy) / denom;
    if (slope < static_cast<double>(cfg_.growth_db_per_frame)) return false;
    const double intercept = (sy - slope * sx) / q;
    double dev = 0.0;
    for (int i = 0; i < q; ++i) {
      dev += std::abs(recent(q, i).logmag[static_cast<std::size_t>(k)] - (intercept + slope * i));
    }
    return dev / q <= static_cast<double>(cfg_.growth_max_deviation_db);
  }

  // Peak-to-peak wander of the sub-bin peak position around bin `k` over the
  // stability window, in Hz.
  float freqDeviationHz(int k) const { return freqDeviationHz(k, cfg_.stability_frames); }

  float freqDeviationHz(int k, int frames) const {
    const int q = std::min(frames, static_cast<int>(filled_));
    if (q < 3) return 1e9f;
    float lo = 1e9f, hi = -1e9f;
    for (int age = 0; age < q; ++age) {
      const std::vector<float>& h = recent(q, age).logmag;
      // Track the local maximum within +/-1 bin: a tone that drifts across a bin
      // boundary must not look unstable for that reason alone.
      int kk = k;
      for (int j = std::max(1, k - 1); j <= std::min(static_cast<int>(h.size()) - 2, k + 1); ++j) {
        if (h[static_cast<std::size_t>(j)] > h[static_cast<std::size_t>(kk)]) kk = j;
      }
      const float f = refineFrequencyIn(h, kk);
      lo = std::min(lo, f);
      hi = std::max(hi, f);
    }
    return hi - lo;
  }

  float refineFrequencyIn(const std::vector<float>& logmag, int k) const {
    const float bin_hz = sample_rate_ / static_cast<float>(cfg_.fft_size);
    if (k <= 0 || k + 1 >= static_cast<int>(logmag.size())) return static_cast<float>(k) * bin_hz;
    const std::size_t uk = static_cast<std::size_t>(k);
    const float a = logmag[uk - 1], b = logmag[uk], c = logmag[uk + 1];
    const float denom = a - 2.0f * b + c;
    float delta = 0.0f;
    if (std::abs(denom) > 1e-9f) delta = 0.5f * (a - c) / denom;
    delta = std::max(-0.5f, std::min(0.5f, delta));
    return (static_cast<float>(k) + delta) * bin_hz;
  }

  // Parabolic interpolation on the log spectrum for sub-bin frequency accuracy.
  float refineFrequency(int k) const {
    const float bin_hz = sample_rate_ / static_cast<float>(cfg_.fft_size);
    if (k <= 0 || k + 1 >= static_cast<int>(logmag_.size())) return static_cast<float>(k) * bin_hz;
    const std::size_t uk = static_cast<std::size_t>(k);
    const float a = logmag_[uk - 1], b = logmag_[uk], c = logmag_[uk + 1];
    const float denom = a - 2.0f * b + c;
    float delta = 0.0f;
    if (std::abs(denom) > 1e-9f) delta = 0.5f * (a - c) / denom;
    delta = std::max(-0.5f, std::min(0.5f, delta));
    return (static_cast<float>(k) + delta) * bin_hz;
  }

  DetectorConfig cfg_;
  float sample_rate_;
  float frame_rate_;
  Fft fft_;
  std::vector<float> window_;
  std::vector<float> frame_;
  std::vector<float> windowed_;
  std::vector<float> mag_;
  std::vector<float> logmag_;
  std::vector<Frame> history_;  // preallocated ring, never resized after construction
  std::size_t head_ = 0;        // next slot to overwrite
  std::size_t filled_ = 0;      // valid frames so far
  std::vector<HowlingCandidate> candidates_;
  long fired_fast_ = 0, fired_growth_ = 0, fired_ipmp_ = 0;
};

}  // namespace ahs
