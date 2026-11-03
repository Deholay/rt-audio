#pragma once

#include <rt_audio/project2/engine.hpp>
#include <rt_audio/project2/processor.hpp>

#include <memory>
#include <span>
#include <string_view>

namespace rt_audio::ahs {

// One vendored AHS snapshot under release/ahs/ahs-<name>/.
struct Release {
    std::string_view name;          // e.g. "v7-c0291dd1e"; selects the release
    std::string_view source_commit; // full AHS commit the files were copied from
    std::string_view source_branch;
    std::string_view configuration; // AHS engine configuration the release runs
    // "callback": whole AHS engine inside the audio callback (bit-identical to AHS).
    // "split": detector on the analysis worker, AHS types over the SPSC queues.
    std::string_view architecture;
};

// Oldest first; the last entry is the newest release.
[[nodiscard]] std::span<const Release> releases() noexcept;

// One running AHS stream: its engine plus whatever analysis worker the release
// needs. Order: start(), then the device; stop the device, then stop().
// Destroy only after both are stopped.
class Session {
public:
    virtual ~Session() = default;
    // Hand this to PortAudioDevice or a simulator; valid for the session's life.
    [[nodiscard]] virtual AudioCallback& callback() noexcept = 0;
    virtual void start() = 0;
    virtual void stop() noexcept = 0;
    // Call after stop(); rethrows an analysis worker failure.
    virtual void rethrow_if_failed() const = 0;
};

// Works for every release. Requires 48000 Hz; the engine prepares on creation.
// output_gain matches AHS --gain (its default 0.1 = -20 dB), range [0, 1].
// Name "latest" selects the newest release. Throws std::invalid_argument for
// an unknown name, an out-of-range gain or an unsupported stream format.
[[nodiscard]] std::unique_ptr<Session> make_session(
    std::string_view name, const StreamConfig& config, float output_gain = 0.1F);

// "callback" releases only: the whole AHS engine as a RealtimeProcessor for
// your own Project2. The loudspeaker reference is the processor's own output.
// ParameterSnapshot commands are ignored. Here "latest" means the newest
// "callback" release. Throws std::invalid_argument for a "split" release,
// which needs make_session().
[[nodiscard]] std::unique_ptr<RealtimeProcessor> make_processor(
    std::string_view name = "latest", float output_gain = 0.1F);

} // namespace rt_audio::ahs
