#pragma once

#include <quant/ipc/spsc_ring.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <thread>

namespace rt_audio {

inline constexpr std::uint32_t max_frames = 2048;
inline constexpr std::uint32_t max_channels = 2;
inline constexpr std::size_t max_notches = 16;
inline constexpr std::size_t analysis_capacity = 32;
inline constexpr std::size_t command_capacity = 32;
inline constexpr std::size_t max_commands_per_callback = 4;

struct StreamConfig {
    double sample_rate = 48000.0;
    std::uint32_t channels = 1;
    std::uint32_t frames_per_buffer = 128; // nominal; actual callbacks may vary
};

// Flags intentionally match PortAudio. Other backends must translate theirs.
enum CallbackStatus : std::uint32_t {
    InputUnderflow = 1,
    InputOverflow = 2,
    OutputUnderflow = 4,
    OutputOverflow = 8,
    PrimingOutput = 16,
};

struct CallbackInfo {
    double input_time_seconds = 0.0;
    double output_time_seconds = 0.0;
    std::uint32_t status_flags = 0;
};

// Interleaved float32; frame = one sample per channel. Only frames * channels
// entries are valid. The queue owns a COPY, never the driver's buffer pointer.
struct AnalysisBlock {
    // Delivered frames per channel, not float offset or hardware sample clock.
    // Hardware discontinuities additionally require status_flags/timestamps.
    std::uint64_t first_sample = 0;
    double input_time_seconds = 0.0;
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::uint32_t status_flags = 0;
    std::uint32_t input_missing = 0;
    std::array<float, max_frames * max_channels> samples{};
};

struct NotchTarget {
    float frequency_hz = 1000.0F;
    float bandwidth_hz = 100.0F;
    float depth_db = 0.0F; // nonnegative attenuation, 0 = no attenuation
};

// One complete configuration, not a delta. A callback may coalesce snapshots.
// The algorithm plugin owns coefficient calculation and parameter smoothing.
struct ParameterSnapshot {
    std::uint64_t revision = 0; // strictly increasing, nonzero, per engine lifetime
    std::uint32_t notch_count = 0;
    std::uint32_t bypass = 0; // 0 or 1
    float transition_ms = 5.0F;
    std::array<NotchTarget, max_notches> notches{};
};

struct ProcessBlock {
    const float* input;
    float* output;
    std::uint32_t frames;
    std::uint32_t channels;
    double sample_rate;
    std::uint64_t first_sample;
    CallbackInfo timing;
};

class RealtimeProcessor {
public:
    virtual ~RealtimeProcessor() = default;
    // Control thread, before streaming: allocate filters/state here. Scratch
    // must support up to max_frames, not just nominal frames_per_buffer.
    virtual void prepare(const StreamConfig& config) = 0;
    // Audio thread, once at most per block, before process(). No allocation,
    // locks, I/O, waiting or exceptions. Copy targets; do not retain references.
    virtual void apply_parameters(const ParameterSnapshot& parameters) noexcept = 0;
    // Audio thread: fill EVERY output sample, for 1..max_frames. Input/output
    // must be disjoint or exactly identical, never partially overlapping.
    // References and sample pointers are valid only for this invocation.
    virtual void process(const ProcessBlock& block) noexcept = 0;
};

// Wiring placeholder only: does not implement howling detection, IIR or smoothing.
class PassthroughProcessor final : public RealtimeProcessor {
public:
    void prepare(const StreamConfig&) override {}
    void apply_parameters(const ParameterSnapshot&) noexcept override {}
    void process(const ProcessBlock& block) noexcept override;
};

class ParameterSink {
public:
    virtual ~ParameterSink() = default;
    // Sole producer: analysis worker. False means invalid/reordered command or
    // full queue. Caller retains ownership and may retry a full-queue failure.
    [[nodiscard]] virtual bool try_submit_parameters(
        const ParameterSnapshot& parameters) noexcept = 0;
};

struct Statistics {
    std::uint64_t callbacks;
    std::uint64_t processed_frames;
    std::uint64_t analysis_dropped;
    std::uint64_t commands_dropped;
    std::uint64_t commands_rejected;
    std::uint64_t commands_applied;
    std::uint64_t input_missing;
    std::uint64_t invalid_callbacks;
    std::uint64_t input_overflows;
    std::uint64_t output_underflows;
};

class Project2 final : public ParameterSink {
public:
    // Construct and destroy only while both queue endpoints are stopped.
    // Processor must outlive this engine and must not be shared with a worker.
    Project2(StreamConfig config, RealtimeProcessor& processor);
    Project2(const Project2&) = delete;
    Project2& operator=(const Project2&) = delete;

    // Sole audio callback endpoint; valid range 1..max_frames, nonnull output.
    // A null input produces silence as DSP input. Invalid calls return false
    // without touching buffers; the device adapter must silence/abort safely.
    [[nodiscard]] bool process_callback(const float* input, float* output,
                                        std::uint32_t frames,
                                        CallbackInfo info = {}) noexcept;

    // Sole analysis consumer. Do not also call this when AnalysisWorker runs.
    [[nodiscard]] bool try_read_analysis(AnalysisBlock& block) noexcept;
    // Sole parameter producer; UI/control writers must route through this owner.
    [[nodiscard]] bool try_submit_parameters(
        const ParameterSnapshot& parameters) noexcept override;

    [[nodiscard]] const StreamConfig& config() const noexcept { return config_; }
    // Independently sampled counters; not an atomic multi-counter snapshot.
    [[nodiscard]] Statistics statistics() const noexcept;

private:
    bool valid_parameters(const ParameterSnapshot& parameters) const noexcept;
    StreamConfig config_;
    RealtimeProcessor& processor_;
    quant::ipc::SpscRing<AnalysisBlock, analysis_capacity> analysis_;
    quant::ipc::SpscRing<ParameterSnapshot, command_capacity> commands_;
    AnalysisBlock capture_{};
    std::array<float, max_frames * max_channels> silence_{};
    std::uint64_t next_sample_ = 0; // audio thread only
    std::uint64_t last_submitted_revision_ = 0; // parameter producer only
    std::atomic<std::uint64_t> callbacks_{0}, processed_frames_{0},
        analysis_dropped_{0}, commands_dropped_{0}, commands_rejected_{0},
        commands_applied_{0}, input_missing_{0}, invalid_callbacks_{0},
        input_overflows_{0}, output_underflows_{0};
};

class AnalysisHandler {
public:
    virtual ~AnalysisHandler() = default;
    virtual void prepare(const StreamConfig&) {}
    // Background worker. Assemble rolling windows here; inspect first_sample
    // for queue gaps, status/timing for device gaps. Block is reused on the next
    // iteration: do not retain references. Handler must return for stop() to join.
    // Only this endpoint may publish to the supplied parameter sink.
    virtual void on_audio(const AnalysisBlock& block, ParameterSink& output) = 0;
};

class NoopAnalysisHandler final : public AnalysisHandler {
public:
    void on_audio(const AnalysisBlock&, ParameterSink&) override {}
};

class AnalysisWorker final {
public:
    AnalysisWorker(Project2& engine, AnalysisHandler& handler) noexcept;
    ~AnalysisWorker();
    AnalysisWorker(const AnalysisWorker&) = delete;
    AnalysisWorker& operator=(const AnalysisWorker&) = delete;
    // Control-thread lifecycle only. One start per worker instance.
    void start();
    void stop() noexcept;
    [[nodiscard]] bool failed() const noexcept;
    // Call after stop(); propagates detector errors outside the audio callback.
    void rethrow_if_failed() const;

private:
    void run() noexcept;
    Project2& engine_;
    AnalysisHandler& handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    bool started_ = false;
    std::thread thread_;
    std::exception_ptr failure_;
};

} // namespace rt_audio
