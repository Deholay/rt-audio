#include <rt_audio/project2/portaudio_device.hpp>
#include <rt_audio/project2/engine.hpp>

#include <portaudio.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace rt_audio {
namespace {

void check_pa(PaError error, std::string_view operation) {
    if (error != paNoError) {
        throw std::runtime_error(std::string(operation) + ": " + Pa_GetErrorText(error));
    }
}

class PortAudioRuntime {
public:
    PortAudioRuntime() { check_pa(Pa_Initialize(), "Pa_Initialize"); }
    ~PortAudioRuntime() { (void)Pa_Terminate(); }
    PortAudioRuntime(const PortAudioRuntime&) = delete;
    PortAudioRuntime& operator=(const PortAudioRuntime&) = delete;
};

class AudioStream {
public:
    PaStream* handle = nullptr;

    ~AudioStream() {
        if (handle != nullptr) {
            (void)Pa_AbortStream(handle);
            (void)Pa_CloseStream(handle);
        }
    }
    AudioStream() = default;
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;
};

struct CallbackContext {
    AudioCallback& engine;
    std::uint32_t channels;
    std::atomic<bool> aborted{false};
};
static_assert(std::atomic<bool>::is_always_lock_free);

int audio_callback(const void* input, void* output, unsigned long frames,
                   const PaStreamCallbackTimeInfo* time_info,
                   PaStreamCallbackFlags status_flags, void* user_data) noexcept {
    auto& context = *static_cast<CallbackContext*>(user_data);
    CallbackInfo info{};
    if (time_info != nullptr) {
        info.input_time_seconds = time_info->inputBufferAdcTime;
        info.output_time_seconds = time_info->outputBufferDacTime;
    }
    constexpr PaStreamCallbackFlags portable_flags =
        paInputUnderflow | paInputOverflow | paOutputUnderflow | paOutputOverflow | paPrimingOutput;
    info.status_flags = static_cast<std::uint32_t>(status_flags & portable_flags);
    if (frames <= std::numeric_limits<std::uint32_t>::max() &&
        context.engine.process_callback(static_cast<const float*>(input), static_cast<float*>(output),
                                        static_cast<std::uint32_t>(frames), info)) {
        return paContinue;
    }
    // PortAudio owns an output buffer of exactly frames * channels samples.
    if (output != nullptr && frames <= std::numeric_limits<std::size_t>::max() / context.channels) {
        std::fill_n(static_cast<float*>(output), static_cast<std::size_t>(frames) * context.channels, 0.0F);
    }
    context.aborted.store(true, std::memory_order_relaxed);
    return paAbort;
}

PaStreamParameters device_parameters(int requested_device, bool input, std::uint32_t channels,
                                     double suggested_latency) {
    PaStreamParameters parameters{};
    parameters.device = requested_device == -1
        ? (input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice()) : requested_device;
    if (parameters.device == paNoDevice) {
        throw std::runtime_error(input ? "No default input device" : "No default output device");
    }
    const auto* device = Pa_GetDeviceInfo(parameters.device);
    if (device == nullptr) throw std::invalid_argument("Invalid device index; use --list-devices");
    const int available = input ? device->maxInputChannels : device->maxOutputChannels;
    if (static_cast<int>(channels) > available) {
        throw std::invalid_argument(input ? "Input device has insufficient channels" : "Output device has insufficient channels");
    }
    parameters.channelCount = static_cast<int>(channels);
    parameters.sampleFormat = paFloat32;
    parameters.suggestedLatency = suggested_latency > 0.0 ? suggested_latency
        : input ? device->defaultLowInputLatency : device->defaultLowOutputLatency;
    return parameters;
}

} // namespace

struct PortAudioDevice::Impl {
    // Reverse destruction closes the stream before releasing callback storage
    // or terminating the PortAudio runtime, including constructor failure paths.
    PortAudioRuntime runtime;
    CallbackContext context;
    AudioStream stream;
    DeviceStreamInfo information;

    Impl(AudioCallback& engine, DeviceSelection selection)
        : context{engine, engine.config().channels} {
        const auto& config = engine.config();
        if (!(selection.suggested_latency >= 0.0 && selection.suggested_latency <= 1.0)) {
            throw std::invalid_argument("Suggested latency must be within 0..1 seconds");
        }
        auto input = device_parameters(selection.input_device, true, config.channels,
                                       selection.suggested_latency);
        auto output = device_parameters(selection.output_device, false, config.channels,
                                        selection.suggested_latency);
        check_pa(Pa_IsFormatSupported(&input, &output, config.sample_rate), "Requested duplex format");
        information.input_name = Pa_GetDeviceInfo(input.device)->name;
        information.output_name = Pa_GetDeviceInfo(output.device)->name;
        information.requested = config;
        information.suggested_input_latency = input.suggestedLatency;
        information.suggested_output_latency = output.suggestedLatency;
        check_pa(Pa_OpenStream(&stream.handle, &input, &output, config.sample_rate,
                              config.frames_per_buffer, paNoFlag, audio_callback, &context), "Pa_OpenStream");
        if (const auto* reported = Pa_GetStreamInfo(stream.handle)) {
            information.has_reported_latency = true;
            information.sample_rate = reported->sampleRate;
            information.input_latency = reported->inputLatency;
            information.output_latency = reported->outputLatency;
        }
    }
};

PortAudioDevice::PortAudioDevice(AudioCallback& engine, DeviceSelection selection)
    : impl_(std::make_unique<Impl>(engine, selection)) {}

PortAudioDevice::~PortAudioDevice() = default;

void PortAudioDevice::start() {
    check_pa(Pa_StartStream(impl_->stream.handle), "Pa_StartStream");
}

void PortAudioDevice::stop() {
    const auto error = Pa_StopStream(impl_->stream.handle);
    if (error != paStreamIsStopped) check_pa(error, "Pa_StopStream");
}

bool PortAudioDevice::is_active() const {
    const auto active = Pa_IsStreamActive(impl_->stream.handle);
    if (active < 0) check_pa(active, "Pa_IsStreamActive");
    return active != 0;
}

bool PortAudioDevice::aborted() const noexcept {
    return impl_->context.aborted.load(std::memory_order_relaxed);
}

const DeviceStreamInfo& PortAudioDevice::info() const noexcept {
    return impl_->information;
}

std::vector<DeviceInfo> PortAudioDevice::list_devices() {
    PortAudioRuntime runtime;
    const int count = Pa_GetDeviceCount();
    if (count < 0) check_pa(count, "Pa_GetDeviceCount");
    const auto default_input = Pa_GetDefaultInputDevice();
    const auto default_output = Pa_GetDefaultOutputDevice();
    std::vector<DeviceInfo> devices;
    devices.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const auto* device = Pa_GetDeviceInfo(index);
        if (device == nullptr) continue;
        const auto* host = Pa_GetHostApiInfo(device->hostApi);
        devices.push_back(DeviceInfo{index, device->name, host ? host->name : "unknown host",
            device->maxInputChannels, device->maxOutputChannels, device->defaultSampleRate,
            index == default_input, index == default_output});
    }
    return devices;
}

} // namespace rt_audio
