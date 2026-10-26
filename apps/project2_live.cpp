#include <rt_audio/project2.hpp>

#include <rt_audio/project2/portaudio_device.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
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
    int input_device = -1;
    int output_device = -1;
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

void list_devices() {
    const auto devices = rt_audio::PortAudioDevice::list_devices();
    for (const auto& device : devices) {
        std::cout << device.index << ": " << device.name << " [" << device.host_api
                  << "] input=" << device.max_input_channels << " output=" << device.max_output_channels
                  << " default_rate=" << device.default_sample_rate;
        if (device.is_default_input) std::cout << " [default input]";
        if (device.is_default_output) std::cout << " [default output]";
        std::cout << '\n';
    }
    if (devices.empty()) std::cout << "No audio devices available.\n";
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
    if (options.list_devices) {
        list_devices();
        return 0;
    }

    rt_audio::PassthroughProcessor processor;
    auto engine = std::make_unique<rt_audio::Project2>(options.config, processor);
    rt_audio::NoopAnalysisHandler handler;
    rt_audio::AnalysisWorker worker(*engine, handler);
    // Declared after the worker: exception unwinding stops device callbacks first.
    rt_audio::PortAudioDevice device(*engine, {options.input_device, options.output_device});
    const auto& info = device.info();
    std::cout << "Input: " << info.input_name
              << "\nOutput: " << info.output_name
              << "\nRequested: " << info.requested.sample_rate << " Hz, " << info.requested.channels
              << " channel(s), " << info.requested.frames_per_buffer << " frames/callback"
              << "\nSuggested latency: input=" << info.suggested_input_latency * 1000.0
              << " ms output=" << info.suggested_output_latency * 1000.0 << " ms\n";
    if (info.has_reported_latency) {
        std::cout << "PortAudio reported: " << info.sample_rate << " Hz, input=" << info.input_latency * 1000.0
                  << " ms output=" << info.output_latency * 1000.0
                  << " ms (not a measured acoustic round-trip latency)\n";
    }
    std::cout << "Passthrough only. Use headphones. Running for " << options.seconds << " seconds; Ctrl-C to stop.\n";

    std::signal(SIGINT, request_stop);
    worker.start();
    device.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(options.seconds);
    bool stopped_early = false;
    while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
        if (!device.is_active()) {
            stopped_early = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    device.stop();
    worker.stop();
    print_statistics(engine->statistics());
    worker.rethrow_if_failed();
    if (device.aborted()) {
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
