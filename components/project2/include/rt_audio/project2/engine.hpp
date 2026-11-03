#pragma once

#include <rt_audio/project2/analysis.hpp>
#include <rt_audio/project2/processor.hpp>
#include <rt_audio/project2/detail/spsc_ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rt_audio {

// Device-facing side of an engine; what PortAudioDevice and simulators drive.
class AudioCallback {
public:
    virtual ~AudioCallback() = default;
    // Sole audio callback endpoint; valid range 1..max_frames, nonnull output.
    // A null input produces silence as DSP input. Invalid calls return false
    // without touching buffers; the device adapter must silence/abort safely.
    [[nodiscard]] virtual bool process_callback(const float* input, float* output,
                                                std::uint32_t frames,
                                                CallbackInfo info = {}) noexcept = 0;
    [[nodiscard]] virtual const StreamConfig& config() const noexcept = 0;
    // Independently sampled counters; not an atomic multi-counter snapshot.
    [[nodiscard]] virtual Statistics statistics() const noexcept = 0;
};

template <class Protocol>
class Engine;

// Audio-thread view of an engine's two queues, handed to Protocol::callback().
template <class Protocol>
class CallbackPort {
public:
    // False when the analysis queue is full; counted as analysis_dropped.
    [[nodiscard]] bool push_tap(const typename Protocol::Tap& tap) noexcept;
    // Same, but fill(Tap&) writes straight into the queue slot (no staging copy).
    // fill must be noexcept, write every field the consumer reads, and is not
    // called when the queue is full. The slot holds stale data from earlier use.
    template <class Fill>
    [[nodiscard]] bool write_tap(Fill&& fill) noexcept;
    [[nodiscard]] bool pop_command(typename Protocol::Command& command) noexcept;
    void command_applied() noexcept;

private:
    friend class Engine<Protocol>;
    explicit CallbackPort(Engine<Protocol>& engine) noexcept : engine_(engine) {}
    Engine<Protocol>& engine_;
};

// A protocol fixes what crosses the two SPSC queues and who produces it:
//   Tap, Command                 trivially copyable queue payloads
//   Processor, Handler, Sink     callback DSP, worker analysis, command port type
//                                (Sink derives CommandSink<Command>)
//   tap_capacity, command_capacity   power-of-two queue sizes
//   static bool valid(const Command&, const StreamConfig&) noexcept
//   static std::uint64_t sequence(const Command&) noexcept  // nonzero, increasing
//   static void deliver(Handler&, const Tap&, Sink&)        // worker thread
//   void callback(CallbackPort&, Processor&, const ProcessBlock&,
//                 bool input_missing) noexcept               // audio thread
//
// Default protocol: the engine copies raw input to the analysis queue before
// DSP; full ParameterSnapshot commands are coalesced, newest applied per block.
struct DefaultProtocol {
    using Tap = AnalysisBlock;
    using Command = ParameterSnapshot;
    using Processor = RealtimeProcessor;
    using Handler = AnalysisHandler;
    using Sink = ParameterSink;
    static constexpr std::size_t tap_capacity = analysis_capacity;
    static constexpr std::size_t command_capacity = rt_audio::command_capacity;

    static bool valid(const ParameterSnapshot& parameters, const StreamConfig& config) noexcept;
    static std::uint64_t sequence(const ParameterSnapshot& parameters) noexcept {
        return parameters.revision;
    }
    static void deliver(AnalysisHandler& handler, const AnalysisBlock& block, ParameterSink& sink) {
        handler.on_audio(block, sink);
    }
    void callback(CallbackPort<DefaultProtocol>& port, RealtimeProcessor& processor,
                  const ProcessBlock& block, bool input_missing) noexcept;
};

template <class Protocol>
class Engine final : public AudioCallback, public Protocol::Sink {
public:
    using Tap = typename Protocol::Tap;
    using Command = typename Protocol::Command;
    using Processor = typename Protocol::Processor;

    // Construct and destroy only while both queue endpoints are stopped.
    // Processor must outlive this engine and must not be shared with a worker.
    Engine(StreamConfig config, Processor& processor);
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] bool process_callback(const float* input, float* output,
                                        std::uint32_t frames,
                                        CallbackInfo info = {}) noexcept override;

    // Sole analysis consumer. Do not also call this when a worker runs.
    [[nodiscard]] bool try_read_analysis(Tap& tap) noexcept;
    // Sole command producer; UI/control writers must route through this owner.
    [[nodiscard]] bool try_submit(const Command& command) noexcept override;

    [[nodiscard]] const StreamConfig& config() const noexcept override { return config_; }
    [[nodiscard]] Statistics statistics() const noexcept override;

private:
    friend class CallbackPort<Protocol>;
    StreamConfig config_;
    Processor& processor_;
    Protocol protocol_{};
    detail::SpscRing<Tap, Protocol::tap_capacity> taps_;
    detail::SpscRing<Command, Protocol::command_capacity> commands_;
    std::array<float, max_frames * max_channels> silence_{};
    std::uint64_t next_sample_ = 0; // audio thread only
    std::uint64_t last_submitted_sequence_ = 0; // command producer only
    std::atomic<std::uint64_t> callbacks_{0}, processed_frames_{0},
        analysis_dropped_{0}, commands_dropped_{0}, commands_rejected_{0},
        commands_applied_{0}, input_missing_{0}, invalid_callbacks_{0},
        input_overflows_{0}, output_underflows_{0};
};

// Definitions live in detail/engine_impl.hpp; engine.cpp instantiates this one.
using Project2 = Engine<DefaultProtocol>;
extern template class Engine<DefaultProtocol>;
extern template class CallbackPort<DefaultProtocol>;

} // namespace rt_audio
