#pragma once

#include <rt_audio/project2/types.hpp>

namespace rt_audio {

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

} // namespace rt_audio
