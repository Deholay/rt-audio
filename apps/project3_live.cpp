#include <rt_audio/project3.hpp>

#include <rt_audio/project3/portaudio_device.hpp>

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
    rt_audio::project3::StreamConfig config{};
    int input_device = -1;
    int output_device = -1;
    double seconds = 10.0;
    bool list_devices = false;
    bool help = false;
};

void print_usage() {
    std::cout
        << "Project 3 microphone -> input SPSC -> worker -> output SPSC -> aligned playback\n"
        << "The supplied processor is passthrough; suppression is an extension port.\n\n"
        << "Usage: project3_live [options]\n"
        << "  --help                 Show this help without opening audio devices\n"
        << "  --list-devices         Enumerate PortAudio devices and exit\n"
        << "  --input N              Input device index (default: system default)\n"
        << "  --output N             Output device index (default: system default)\n"
        << "  --channels 1|2         Interleaved channels on both endpoints (default: 1)\n"
        << "  --sample-rate HZ       Sample rate, 8000..384000 (default: 48000)\n"
        << "  --frames N             Frames per callback, 1..2048 (default: 128)\n"
        << "  --block-frames N        Worker hop size, 1..2048 (default: 128)\n"
        << "  --lookahead-frames N    Algorithm lookahead budget (default: 256)\n"
        << "  --delay-frames N        Fixed software delay, <=16384 (default: 512)\n"
        << "  --seconds N            Run duration, >0 and <=86400 (default: 10)\n"
        << "Ctrl-C stops playback and joins the processing worker.\n"
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
                argument != "--sample-rate" && argument != "--frames" && argument != "--seconds" &&
                argument != "--block-frames" && argument != "--lookahead-frames" && argument != "--delay-frames") {
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
            if (argument == "--block-frames") options.config.block_frames = static_cast<std::uint32_t>(parse_integer(value, argument));
            if (argument == "--lookahead-frames") options.config.lookahead_frames = static_cast<std::uint32_t>(parse_integer(value, argument));
            if (argument == "--delay-frames") options.config.delay_frames = static_cast<std::uint32_t>(parse_integer(value, argument));
            if (argument == "--sample-rate") options.config.sample_rate = parse_number(value, argument);
            if (argument == "--seconds") options.seconds = parse_number(value, argument);
        }
    }
    if (options.config.channels < 1 || options.config.channels > rt_audio::project3::max_channels) {
        throw std::invalid_argument("--channels must be 1 or 2");
    }
    if (options.config.frames_per_buffer < 1 || options.config.frames_per_buffer > rt_audio::project3::max_frames) {
        throw std::invalid_argument("--frames must be between 1 and 2048");
    }
    if (options.config.sample_rate < 8000.0 || options.config.sample_rate > 384000.0) {
        throw std::invalid_argument("--sample-rate must be between 8000 and 384000 Hz");
    }
    if (options.config.block_frames == 0 || options.config.block_frames > rt_audio::project3::max_frames ||
        options.config.lookahead_frames > rt_audio::project3::max_delay_frames ||
        options.config.delay_frames > rt_audio::project3::max_delay_frames ||
        options.config.delay_frames < options.config.block_frames + options.config.lookahead_frames) {
        throw std::invalid_argument("Delay must cover a valid hop plus lookahead, and be <=16384 frames");
    }
    if (options.seconds > 86400.0) throw std::invalid_argument("--seconds exceeds 86400");
    return options;
}

void list_devices() {
    const auto devices = rt_audio::project3::PortAudioDevice::list_devices();
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

void print_statistics(const rt_audio::project3::Statistics& stats) {
    std::cout << "callbacks=" << stats.callbacks << " input_frames=" << stats.captured_frames
              << " startup_frames=" << stats.startup_frames
              << " processed_frames=" << stats.processed_frames << " fallback_frames=" << stats.fallback_frames
              << " input_dropped=" << stats.input_dropped << " output_dropped=" << stats.output_dropped
              << " output_late=" << stats.output_late << " output_stale=" << stats.output_stale
              << " output_rejected=" << stats.output_rejected
              << " input_overflows=" << stats.input_overflows
              << " output_underflows=" << stats.output_underflows << '\n';
}

int run(const Options& options) {
    if (options.list_devices) {
        list_devices();
        return 0;
    }

    rt_audio::project3::PassthroughProcessor processor;
    auto engine = std::make_unique<rt_audio::project3::Engine>(options.config);
    rt_audio::project3::ProcessingWorker worker(*engine, processor);
    // Declared after the worker: exception unwinding stops device callbacks first.
    rt_audio::project3::PortAudioDevice device(*engine, {options.input_device, options.output_device});
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
    const auto software_ms = 1000.0 * options.config.delay_frames / options.config.sample_rate;
    std::cout << "Fixed software delay=" << software_ms << " ms; hop=" << options.config.block_frames
              << " frames, reserved lookahead=" << options.config.lookahead_frames << " frames\n";
    if (info.has_reported_latency) {
        std::cout << "Device-reported latency plus software delay="
                  << software_ms + 1000.0 * (info.input_latency + info.output_latency)
                  << " ms (estimate only; the <=30 ms target is not verified)\n";
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
        std::cerr << "project3_live: " << error.what() << '\n';
        return 1;
    }
}
