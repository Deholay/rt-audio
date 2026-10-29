#include <rt_audio/project3.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace p3 = rt_audio::project3;

float sample(std::uint64_t n) { return (static_cast<float>(n % 251) - 125.0F) / 256.0F; }

int main() {
    try {
        p3::StreamConfig config;
        auto engine = std::make_unique<p3::Engine>(config);
        p3::PassthroughProcessor processor;
        p3::ProcessingWorker worker(*engine, processor);
        std::array<float, 128> input{}, output{};
        worker.start();
        for (std::uint64_t block = 0; block < 256; ++block) {
            // Deliberately remove the worker halfway through to exercise fallback.
            if (block == 128) { worker.stop(); worker.rethrow_if_failed(); }
            for (std::size_t n = 0; n < input.size(); ++n) input[n] = sample(block * 128 + n);
            if (!engine->process_callback(input.data(), output.data(), 128)) throw std::runtime_error("Callback rejected");
            for (std::size_t n = 0; n < output.size(); ++n) {
                const auto time = block * 128 + n;
                const float expected = time < config.delay_frames ? 0.0F : sample(time - config.delay_frames);
                if (output[n] != expected) throw std::runtime_error("Playback alignment changed");
            }
            if (block < 128) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto s = engine->statistics();
        if (s.processed_frames == 0 || s.fallback_frames == 0) throw std::runtime_error("Expected both processed and fallback routes");
        std::cout << "PASS: input SPSC -> worker -> output SPSC, with sample-aligned dry fallback\n"
                  << "delay_frames=" << config.delay_frames << " processed_frames=" << s.processed_frames
                  << " fallback_frames=" << s.fallback_frames << " input_dropped=" << s.input_dropped << '\n'
                  << "No microphone/speaker opened. No suppression algorithm installed.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
