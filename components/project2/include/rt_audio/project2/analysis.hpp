#pragma once

#include <rt_audio/project2/types.hpp>

namespace rt_audio {

class ParameterSink {
public:
    virtual ~ParameterSink() = default;
    // Sole producer: analysis worker. False means invalid/reordered command or
    // full queue. Caller retains ownership and may retry a full-queue failure.
    [[nodiscard]] virtual bool try_submit_parameters(
        const ParameterSnapshot& parameters) noexcept = 0;
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

} // namespace rt_audio
