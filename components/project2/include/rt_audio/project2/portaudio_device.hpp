#pragma once

#include <rt_audio/project2/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace rt_audio {

class AudioCallback;

struct DeviceSelection {
    int input_device = -1; // -1 selects the system default
    int output_device = -1;
    // Device buffering request in seconds, applied to both directions; 0 uses
    // each device's default low latency. PortAudio may round it; read the
    // result from DeviceStreamInfo::input_latency / output_latency.
    double suggested_latency = 0.0;
};

struct DeviceInfo {
    int index;
    std::string name;
    std::string host_api;
    int max_input_channels;
    int max_output_channels;
    double default_sample_rate;
    bool is_default_input;
    bool is_default_output;
};

struct DeviceStreamInfo {
    std::string input_name;
    std::string output_name;
    StreamConfig requested;
    double suggested_input_latency = 0.0; // seconds
    double suggested_output_latency = 0.0;
    bool has_reported_latency = false;
    double sample_rate = 0.0;
    double input_latency = 0.0;
    double output_latency = 0.0;
};

// Optional device adapter. The public API has no PortAudio types or headers.
// Construct/open, enumerate, start, stop and destroy on one control thread.
// Construction opens a stream but never starts it. The engine must outlive this
// object; stop/destroy the device before stopping its analysis worker. Destruction
// aborts/closes the stream even when the caller exits through an exception.
class PortAudioDevice final {
public:
    explicit PortAudioDevice(AudioCallback& engine, DeviceSelection selection = {});
    ~PortAudioDevice();
    PortAudioDevice(const PortAudioDevice&) = delete;
    PortAudioDevice& operator=(const PortAudioDevice&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool is_active() const;
    // True if this stream's callback has rejected a host buffer. Sticky until
    // destruction; does not query the device or call PortAudio.
    [[nodiscard]] bool aborted() const noexcept;
    [[nodiscard]] const DeviceStreamInfo& info() const noexcept;
    // Initializes PortAudio temporarily, copies values, then terminates it.
    // Returns no borrowed PortAudio pointers and does not start any stream.
    [[nodiscard]] static std::vector<DeviceInfo> list_devices();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rt_audio
