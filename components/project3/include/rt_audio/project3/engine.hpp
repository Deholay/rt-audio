#pragma once

#include <rt_audio/project3/detail/spsc_ring.hpp>
#include <rt_audio/project3/processor.hpp>

#include <array>
#include <atomic>

namespace rt_audio::project3 {

class Engine final : public OutputSink {
public:
    explicit Engine(StreamConfig config = {});
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // One duplex audio thread captures input then renders output on one sample
    // clock. Float32 interleaved, 1..max_frames; disjoint or identical buffers,
    // never partial overlap. Null input = silence. Invalid calls do not touch
    // buffers; host must silence/abort. Input/output pointers are not retained.
    // Variable callback sizes are reblocked into config.block_frames hops.
    [[nodiscard]] bool process_callback(const float* input, float* output,
                                        std::uint32_t frames,
                                        CallbackInfo info = {}) noexcept;
    // Sole worker input consumer. Do not call alongside ProcessingWorker.
    [[nodiscard]] bool try_read_input(AudioBlock& block) noexcept;
    [[nodiscard]] bool try_submit_output(const AudioBlock& block) noexcept override;
    [[nodiscard]] std::uint64_t playback_deadline() const noexcept override;
    [[nodiscard]] const StreamConfig& config() const noexcept { return config_; }
    // Independent atomic counters; not a consistent multi-field snapshot.
    [[nodiscard]] Statistics statistics() const noexcept;

private:
    void capture(const float* input, std::uint32_t frames, CallbackInfo info) noexcept;
    void render(float* output, std::uint32_t frames) noexcept;
    void select_block(std::uint64_t first_sample, std::size_t& pop_budget) noexcept;
    StreamConfig config_;
    detail::SpscRing<AudioBlock, queue_capacity> input_queue_;
    detail::SpscRing<AudioBlock, queue_capacity> output_queue_;
    // Dry history is audio-thread-owned. Enough for maximum delay PLUS one
    // entire capture callback, because capture precedes render (also in-place).
    std::array<float, (max_delay_frames + max_frames) * max_channels> dry_{};
    AudioBlock building_{};
    AudioBlock pending_{};
    std::uint32_t building_frames_ = 0;
    std::uint64_t input_cursor_ = 0;
    std::uint64_t output_cursor_ = 0;
    bool have_pending_ = false;
    bool use_processed_ = false;
    std::uint64_t last_submitted_end_ = 0; // worker only
    std::atomic<std::uint64_t> published_input_end_{0}, committed_output_end_{0};
    std::atomic<std::uint64_t> callbacks_{0}, captured_frames_{0}, rendered_frames_{0},
        startup_frames_{0}, processed_frames_{0}, fallback_frames_{0},
        processed_blocks_{0}, fallback_blocks_{0}, input_dropped_{0},
        output_dropped_{0}, output_late_{0}, output_stale_{0}, output_rejected_{0},
        input_missing_{0}, invalid_callbacks_{0}, input_overflows_{0}, output_underflows_{0};
};

} // namespace rt_audio::project3
