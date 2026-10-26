#include <rt_audio/project2.hpp>

#include <portaudio.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) { stop_requested = 1; }

struct Options {
    rt_audio::StreamConfig config{};
    int input_device = paNoDevice;
    int output_device = paNoDevice;
    double seconds = 10.0;
    bool list_devices = false;
    bool help = false;
};

void print_usage() {
    std::cout
        << "Project 2 microphone -> callback processor -> speaker\n"
        << "The supplied processor is passthrough; suppression is an extension port.\n\n"
        << "Usage: project2_live [options]\n"
        << "  --help                 Show this help without opening audio devices\n"
        << "  --list-devices         Enumerate PortAudio devices and exit\n"
        << "  --input N              Input device index (default: system default)\n"
        << "  --output N             Output device index (default: system default)\n"
        << "  --channels 1|2         Interleaved channels on both endpoints (default: 1)\n"
        << "  --sample-rate HZ       Sample rate, 8000..384000 (default: 48000)\n"
        << "  --frames N             Frames per callback, 1..2048 (default: 128)\n"
        << "  --seconds N            Run duration, >0 and <=86400 (default: 10)\n"
        << "Ctrl-C stops playback and joins the analysis worker.\n"
        << "Use headphones for the passthrough test to avoid acoustic feedback.\n";
}

int parse_integer(std::string_view value, std::string_view option) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < 0) {
        throw std::invalid_argument(std::string(option) + " requires a nonnegative integer");
    }
    return result;
}

double parse_number(std::string_view value, std::string_view option) {
    std::size_t consumed = 0;
    double result = 0;
    try {
        result = std::stod(std::string(value), &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(option) + " requires a finite positive number");
    }
    if (consumed != value.size() || !std::isfinite(result) || result <= 0) {
        throw std::invalid_argument(std::string(option) + " requires a finite positive number");
    }
    return result;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            options.help = true;
        } else if (argument == "--list-devices") {
            options.list_devices = true;
        } else {
            if (argument != "--input" && argument != "--output" && argument != "--channels" &&
                argument != "--sample-rate" && argument != "--frames" && argument != "--seconds") {
                throw std::invalid_argument("Unknown option: " + std::string(argument));
            }
            if (++index == argc) {
                throw std::invalid_argument("Missing value for " + std::string(argument));
            }
            const std::string_view value(argv[index]);
            if (argument == "--input") options.input_device = parse_integer(value, argument);
            if (argument == "--output") options.output_device = parse_integer(value, argument);
            if (argument == "--channels") {
                options.config.channels = static_cast<std::uint32_t>(parse_integer(value, argument));
            }
            if (argument == "--frames") {
                options.config.frames_per_buffer = static_cast<std::uint32_t>(parse_integer(value, argument));
            }
            if (argument == "--sample-rate") options.config.sample_rate = parse_number(value, argument);
            if (argument == "--seconds") options.seconds = parse_number(value, argument);
        }
    }
    if (options.config.channels < 1 || options.config.channels > rt_audio::max_channels) {
        throw std::invalid_argument("--channels must be 1 or 2");
    }
    if (options.config.frames_per_buffer < 1 || options.config.frames_per_buffer > rt_audio::max_frames) {
        throw std::invalid_argument("--frames must be between 1 and 2048");
    }
    if (options.config.sample_rate < 8000.0 || options.config.sample_rate > 384000.0) {
        throw std::invalid_argument("--sample-rate must be between 8000 and 384000 Hz");
    }
    if (options.seconds > 86400.0) throw std::invalid_argument("--seconds exceeds 86400");
    return options;
}

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
            // Also covers exception exits. The worker and engine outlive this object.
            (void)Pa_AbortStream(handle);
            (void)Pa_CloseStream(handle);
        }
    }
    AudioStream() = default;
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    void stop() {
        const PaError error = Pa_StopStream(handle);
        if (error != paStreamIsStopped) check_pa(error, "Pa_StopStream");
    }
};

struct CallbackContext {
    rt_audio::Project2& engine;
    std::uint32_t channels;
    std::atomic<bool> aborted{false};
};
static_assert(std::atomic<bool>::is_always_lock_free);

int audio_callback(const void* input, void* output, unsigned long frames,
                   const PaStreamCallbackTimeInfo* time_info,
                   PaStreamCallbackFlags status_flags, void* user_data) noexcept {
    auto& context = *static_cast<CallbackContext*>(user_data);
    rt_audio::CallbackInfo info{};
    if (time_info != nullptr) {
        info.input_time_seconds = time_info->inputBufferAdcTime;
        info.output_time_seconds = time_info->outputBufferDacTime;
    }
    // These are the documented portable PortAudio bits, also used by CallbackInfo.
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

void list_devices() {
    const int count = Pa_GetDeviceCount();
    if (count < 0) check_pa(count, "Pa_GetDeviceCount");
    const auto default_input = Pa_GetDefaultInputDevice();
    const auto default_output = Pa_GetDefaultOutputDevice();
    for (int index = 0; index < count; ++index) {
        const auto* device = Pa_GetDeviceInfo(index);
        if (device == nullptr) continue;
        const auto* host = Pa_GetHostApiInfo(device->hostApi);
        std::cout << index << ": " << device->name << " [" << (host ? host->name : "unknown host")
                  << "] input=" << device->maxInputChannels << " output=" << device->maxOutputChannels
                  << " default_rate=" << device->defaultSampleRate;
        if (index == default_input) std::cout << " [default input]";
        if (index == default_output) std::cout << " [default output]";
        std::cout << '\n';
    }
    if (count == 0) std::cout << "No audio devices available.\n";
}

PaStreamParameters device_parameters(int requested_device, bool input, std::uint32_t channels) {
    PaStreamParameters parameters{};
    parameters.device = requested_device == paNoDevice
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
    parameters.suggestedLatency = input ? device->defaultLowInputLatency : device->defaultLowOutputLatency;
    return parameters;
}

void print_statistics(const rt_audio::Statistics& stats) {
    std::cout << "callbacks=" << stats.callbacks << " frames=" << stats.processed_frames
              << " analysis_dropped=" << stats.analysis_dropped
              << " commands_dropped=" << stats.commands_dropped
              << " commands_rejected=" << stats.commands_rejected
              << " commands_applied=" << stats.commands_applied
              << " input_missing=" << stats.input_missing
              << " invalid_callbacks=" << stats.invalid_callbacks
              << " input_overflows=" << stats.input_overflows
              << " output_underflows=" << stats.output_underflows << '\n';
}

int run(const Options& options) {
    PortAudioRuntime runtime;
    if (options.list_devices) {
        list_devices();
        return 0;
    }
    auto input = device_parameters(options.input_device, true, options.config.channels);
    auto output = device_parameters(options.output_device, false, options.config.channels);
    check_pa(Pa_IsFormatSupported(&input, &output, options.config.sample_rate), "Requested duplex format");

    rt_audio::PassthroughProcessor processor;
    auto engine = std::make_unique<rt_audio::Project2>(options.config, processor);
    rt_audio::NoopAnalysisHandler handler;
    rt_audio::AnalysisWorker worker(*engine, handler);
    CallbackContext context{*engine, options.config.channels};
    AudioStream stream;
    check_pa(Pa_OpenStream(&stream.handle, &input, &output, options.config.sample_rate,
                          options.config.frames_per_buffer, paNoFlag, audio_callback, &context), "Pa_OpenStream");

    std::cout << "Input: " << Pa_GetDeviceInfo(input.device)->name
              << "\nOutput: " << Pa_GetDeviceInfo(output.device)->name
              << "\nRequested: " << options.config.sample_rate << " Hz, " << options.config.channels
              << " channel(s), " << options.config.frames_per_buffer << " frames/callback"
              << "\nSuggested latency: input=" << input.suggestedLatency * 1000.0
              << " ms output=" << output.suggestedLatency * 1000.0 << " ms\n";
    if (const auto* info = Pa_GetStreamInfo(stream.handle)) {
        std::cout << "PortAudio reported: " << info->sampleRate << " Hz, input=" << info->inputLatency * 1000.0
                  << " ms output=" << info->outputLatency * 1000.0
                  << " ms (not a measured acoustic round-trip latency)\n";
    }
    std::cout << "Passthrough only. Use headphones. Running for " << options.seconds << " seconds; Ctrl-C to stop.\n";

    std::signal(SIGINT, request_stop);
    worker.start();
    check_pa(Pa_StartStream(stream.handle), "Pa_StartStream");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(options.seconds);
    bool stopped_early = false;
    while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
        const PaError active = Pa_IsStreamActive(stream.handle);
        if (active < 0) check_pa(active, "Pa_IsStreamActive");
        if (active == 0) {
            stopped_early = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    stream.stop();
    worker.stop();
    print_statistics(engine->statistics());
    worker.rethrow_if_failed();
    if (context.aborted.load(std::memory_order_relaxed)) {
        throw std::runtime_error("Audio callback rejected an invalid host buffer");
    }
    if (stopped_early) throw std::runtime_error("Audio device stream stopped before the requested duration");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        if (options.help) {
            print_usage();
            return 0;
        }
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "project2_live: " << error.what() << '\n';
        return 1;
    }
}
