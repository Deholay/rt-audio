#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "biquad.h"

namespace ahs {

struct NotchConfig {
  int max_notches = 5;
  float q = 30.0f;
  float min_depth_db = -3.0f;      // depth applied on the first detection
  float max_depth_db = -18.0f;     // depth after sustained re-detection
  // Depth is bought with detection time rather than with detection count: a
  // notch reaches full depth after (max-min)/rate seconds of uninterrupted
  // detection. Charging per detection instead made the depth a function of the
  // frame rate -- at a 4 ms hop, 24 ms of false detection was enough to put a
  // full-depth notch into clean speech.
  float deepen_db_per_second = -120.0f;  // full depth after 125 ms
  // ...but a notch has to be able to outrun the tone it is fighting, so the
  // rate is raised to this multiple of the tone's own growth rate when the
  // detector reports one. A loop running away at 400 dB/s gets a notch that
  // deepens faster than it climbs; a steady false detection gets the base rate.
  float deepen_growth_factor = 3.0f;
  float max_deepen_db_per_second = 1500.0f;
  // The bank commits to a notch for as long as the evidence for it lasted,
  // between these two bounds. A tone the detector saw for 40 ms does not
  // justify holding a hole in the spectrum for the next two seconds.
  float hold_per_detected_second = 4.0f;
  float min_hold_seconds = 0.15f;
  float hold_seconds = 2.0f;           // cap on the hold for a first detection
  float release_db_per_second = 6.0f;  // how fast a stale notch fades out
  // A notch removes the evidence for itself: once the tone is suppressed the
  // detector stops reporting it, so releasing on "no detection" alone makes the
  // bank let go of a loop that is still critical, and the howl comes back. Each
  // time the same frequency has to be notched again the bank commits to it for
  // longer and lets go of it more slowly, which is what separates a critical
  // loop mode (it recurs) from a one-off false detection (it does not).
  float recurrence_hold_factor = 4.0f;
  float max_hold_seconds = 30.0f;
  // How long a released slot remembers its frequency for that purpose.
  float recurrence_memory_seconds = 3.0f;
  // A re-detection only counts as a recurrence if the detector had lost the
  // tone for at least this long; consecutive frames of one event do not.
  float recurrence_gap_seconds = 0.05f;
  float retune_tolerance_hz = 25.0f;   // detections within this range reuse a slot
};

struct NotchState {
  bool active = false;
  float freq_hz = 0.0f;
  float depth_db = 0.0f;
  float idle_seconds = 0.0f;
  float detected_seconds = 0.0f;  // how long the detector has backed this notch
  int recurrences = 0;            // times the tone came back after being lost
};

// Bank of sample-rate biquad notches. Filtering is sample-by-sample, so the
// suppression path adds no algorithmic latency; only the detection decision is
// frame-rate.
class NotchBank {
 public:
  NotchBank(const NotchConfig& cfg, float sample_rate)
      : cfg_(cfg), sample_rate_(sample_rate),
        filters_(static_cast<std::size_t>(cfg.max_notches)),
        states_(static_cast<std::size_t>(cfg.max_notches)) {
    for (auto& f : filters_) f.setIdentity();
  }

  // Called at frame rate with the frequencies the detector currently believes
  // are howling.
  void update(const std::vector<float>& howling_freqs, float elapsed_seconds) {
    update(howling_freqs, {}, elapsed_seconds);
  }

  // `growth_db_per_s` runs parallel to `howling_freqs`; empty means unknown,
  // which is treated as a tone that is not growing.
  void update(const std::vector<float>& howling_freqs, const std::vector<float>& growth_db_per_s,
              float elapsed_seconds) {
    for (auto& s : states_) {
      s.idle_seconds += elapsed_seconds;
      // Forget a released slot's history once it has been quiet long enough,
      // so an old frequency cannot make an unrelated new detection sticky.
      if (!s.active && s.detected_seconds > 0.0f &&
          s.idle_seconds > cfg_.recurrence_memory_seconds) {
        s.detected_seconds = 0.0f;
        s.recurrences = 0;
        s.freq_hz = 0.0f;
      }
    }

    for (std::size_t fi = 0; fi < howling_freqs.size(); ++fi) {
      const float f = howling_freqs[fi];
      const float growth = fi < growth_db_per_s.size() ? std::max(0.0f, growth_db_per_s[fi]) : 0.0f;
      const float rate = -std::min(cfg_.max_deepen_db_per_second,
                                   std::max(-cfg_.deepen_db_per_second,
                                            cfg_.deepen_growth_factor * growth));
      int slot = findSlot(f);
      if (slot < 0) continue;
      NotchState& s = states_[static_cast<std::size_t>(slot)];
      const bool same_tone =
          s.detected_seconds > 0.0f && std::abs(s.freq_hz - f) <= cfg_.retune_tolerance_hz;
      if (same_tone && s.idle_seconds >= cfg_.recurrence_gap_seconds) ++s.recurrences;
      // A stolen active slot is a new tone, not a small retuning of the old
      // one. Do not carry its frequency, depth, or recurrence into the new tone.
      if (!s.active || !same_tone) {
        if (s.active) ++steals_;  // diagnostic only; nothing downstream reads it
        s.active = true;
        s.freq_hz = f;
        s.depth_db = cfg_.min_depth_db;
        if (!same_tone) {
          s.detected_seconds = 0.0f;
          s.recurrences = 0;
        }
        filters_[static_cast<std::size_t>(slot)].reset();
      } else {
        // Track small drifts of the critical frequency, deepen for as long as
        // the detector keeps backing this frequency.
        s.freq_hz = 0.9f * s.freq_hz + 0.1f * f;
        s.depth_db = std::max(cfg_.max_depth_db, s.depth_db + rate * elapsed_seconds);
      }
      s.detected_seconds += elapsed_seconds;
      s.idle_seconds = 0.0f;
    }

    mergeConvergedSlots();

    for (std::size_t i = 0; i < states_.size(); ++i) {
      NotchState& s = states_[i];
      if (!s.active) continue;
      if (s.idle_seconds > holdSecondsFor(s)) {
        const float release =
            cfg_.release_db_per_second / static_cast<float>(1 + s.recurrences);
        s.depth_db += release * elapsed_seconds;  // toward 0 dB
        if (s.depth_db >= -0.25f) {
          s.active = false;
          s.depth_db = 0.0f;
          filters_[i].setIdentity();
          continue;
        }
      }
      filters_[i].setNotch(s.freq_hz, sample_rate_, cfg_.q, s.depth_db);
    }
  }

  inline float process(float x) {
    for (std::size_t i = 0; i < filters_.size(); ++i) {
      if (states_[i].active) x = filters_[i].process(x);
    }
    return x;
  }

  const std::vector<NotchState>& states() const { return states_; }

  // How many times an active slot was taken over by a different tone. Purely
  // an instrumentation counter, kept out of the filtering path.
  long stealCount() const { return steals_; }

  int activeCount() const {
    int n = 0;
    for (const auto& s : states_) n += s.active ? 1 : 0;
    return n;
  }

 private:
  // How long a notch stays in place after the last detection, proportional to
  // how long the evidence for it lasted.
  float holdSecondsFor(const NotchState& s) const {
    const float sticky = 1.0f + cfg_.recurrence_hold_factor * static_cast<float>(s.recurrences);
    const float hold = cfg_.hold_per_detected_second * s.detected_seconds * sticky;
    return std::max(cfg_.min_hold_seconds,
                    std::min(std::min(cfg_.hold_seconds * sticky, cfg_.max_hold_seconds), hold));
  }

  // Slots track drifting critical frequencies, so two of them can converge onto
  // the same one. Keep the deeper notch and release the duplicate.
  void mergeConvergedSlots() {
    for (std::size_t i = 0; i < states_.size(); ++i) {
      if (!states_[i].active) continue;
      for (std::size_t j = i + 1; j < states_.size(); ++j) {
        if (!states_[j].active) continue;
        if (std::abs(states_[i].freq_hz - states_[j].freq_hz) > cfg_.retune_tolerance_hz) continue;
        const std::size_t keep = states_[i].depth_db <= states_[j].depth_db ? i : j;
        const std::size_t drop = keep == i ? j : i;
        states_[keep].idle_seconds = std::min(states_[i].idle_seconds, states_[j].idle_seconds);
        states_[drop].active = false;
        states_[drop].depth_db = 0.0f;
        filters_[drop].setIdentity();
        if (drop == i) break;
      }
    }
  }

  // Reuse a slot on the same frequency, else take a free one, else steal the
  // shallowest / most idle notch.
  int findSlot(float freq_hz) {
    for (std::size_t i = 0; i < states_.size(); ++i) {
      if (states_[i].active && std::abs(states_[i].freq_hz - freq_hz) <= cfg_.retune_tolerance_hz) {
        return static_cast<int>(i);
      }
    }
    // A released slot that still remembers this frequency comes first, so a
    // recurring loop mode lands back on its own history instead of looking
    // like a fresh detection.
    for (std::size_t i = 0; i < states_.size(); ++i) {
      if (!states_[i].active && states_[i].detected_seconds > 0.0f &&
          std::abs(states_[i].freq_hz - freq_hz) <= cfg_.retune_tolerance_hz) {
        return static_cast<int>(i);
      }
    }
    for (std::size_t i = 0; i < states_.size(); ++i) {
      if (!states_[i].active) return static_cast<int>(i);
    }
    int victim = 0;
    float best = -1.0f;
    for (std::size_t i = 0; i < states_.size(); ++i) {
      const float score = states_[i].idle_seconds - states_[i].depth_db * 0.01f;
      if (score > best) {
        best = score;
        victim = static_cast<int>(i);
      }
    }
    return victim;
  }

  NotchConfig cfg_;
  float sample_rate_;
  std::vector<Biquad> filters_;
  std::vector<NotchState> states_;
  long steals_ = 0;
};

}  // namespace ahs
