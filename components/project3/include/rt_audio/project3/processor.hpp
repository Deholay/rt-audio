#pragma once

#include <rt_audio/project3/types.hpp>

namespace rt_audio::project3 {

class OutputSink {
public:
    virtual ~OutputSink() = default;
    // Sole worker producer. Output must be ordered, non-overlapping, hop-aligned
    // complete blocks of already captured audio. False = dropped/rejected/late.
    // Queue-full failures may be retried; late blocks must be abandoned. True
    // means enqueued, not guaranteed playback: the callback may race the submit.
    [[nodiscard]] virtual bool try_submit_output(const AudioBlock& block) noexcept = 0;
    // First source sample whose playback choice is not yet committed. Advisory;
    // callback can advance it immediately after this read. No waiting is needed.
    [[nodiscard]] virtual std::uint64_t playback_deadline() const noexcept = 0;
};

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    // Control thread before start. Allocate STFT/model/window state here.
    virtual void prepare(const StreamConfig&) {}
    // Background worker ONLY. May emit zero, one or multiple earlier blocks,
    // allowing STFT/lookahead/overlap-add in a plugin. Retain your own window,
    // never this reference or the sink. Return so stop() can join the worker.
    // Reset/reconcile algorithm state on input gaps/status discontinuities.
    virtual void on_audio(const AudioBlock& input, OutputSink& output) = 0;
};

// No suppression algorithm. Publishes the original block and abandons failed
// submissions; the callback's aligned dry path covers missing output.
class PassthroughProcessor final : public AudioProcessor {
public:
    void on_audio(const AudioBlock& input, OutputSink& output) override;
};

} // namespace rt_audio::project3
