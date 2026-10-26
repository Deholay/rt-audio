// Offline notebook driver. The Project 2 callback itself never waits or logs.
#include <rt_audio/project2.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class ObservedProcessor final : public rt_audio::RealtimeProcessor {
public:
    void prepare(const rt_audio::StreamConfig& config) override { passthrough.prepare(config); }
    void apply_parameters(const rt_audio::ParameterSnapshot& parameters) noexcept override {
        applied = parameters;
        passthrough.apply_parameters(parameters);
    }
    void process(const rt_audio::ProcessBlock& block) noexcept override { passthrough.process(block); }
    rt_audio::ParameterSnapshot applied{};
private:
    rt_audio::PassthroughProcessor passthrough;
};

class ObservedAnalysis final : public rt_audio::AnalysisHandler {
public:
    void on_audio(const rt_audio::AnalysisBlock& block, rt_audio::ParameterSink& sink) override {
        if (blocks == 0) {
            head_count = std::min<std::size_t>(head.size(), block.frames);
            std::copy_n(block.samples.begin(), head_count, head.begin());
        }
        for (std::uint32_t i = 0; i < block.frames; ++i) {
            const double sample = block.samples[i];
            squares += sample * sample;
        }
        frames += block.frames;
        if (submitted == 0) {
            // Fixed, zero-attenuation command demonstrates the port, not detection.
            rt_audio::ParameterSnapshot parameters{};
            parameters.revision = 1;
            parameters.notch_count = 1;
            parameters.notches[0] = {1000.0F, 100.0F, 0.0F};
            parameters.transition_ms = 5.0F;
            if (!sink.try_submit_parameters(parameters)) {
                throw std::runtime_error("Demo parameter command was rejected");
            }
            ++submitted;
        }
        completed.store(++blocks, std::memory_order_release);
    }
    std::atomic<std::uint64_t> completed{0};
    std::uint64_t blocks = 0, frames = 0, submitted = 0;
    double squares = 0.0;
    std::array<float, 4> head{};
    std::size_t head_count = 0;
};

std::vector<float> read_audio(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open input audio");
    const auto bytes = file.tellg();
    if (bytes <= 0 || bytes % static_cast<std::streamoff>(sizeof(float)) != 0) {
        throw std::runtime_error("Input must contain nonempty mono float32 samples");
    }
    std::vector<float> samples(static_cast<std::size_t>(bytes) / sizeof(float));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(samples.data()), bytes)) {
        throw std::runtime_error("Cannot read input audio");
    }
    for (float sample : samples) {
        if (!std::isfinite(sample)) throw std::runtime_error("Audio contains NaN or infinity");
    }
    return samples;
}

void print_head(const float* samples, std::size_t count) {
    std::cout << '[';
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) std::cout << ',';
        std::cout << samples[i];
    }
    std::cout << ']';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("Usage: bridge input.f32 output.f32 sample_rate");
        std::size_t parsed = 0;
        const double rate = std::stod(argv[3], &parsed);
        if (parsed != std::string(argv[3]).size()) throw std::invalid_argument("Invalid sample rate");
        const auto input = read_audio(argv[1]);
        std::vector<float> output(input.size());
        ObservedProcessor processor;
        auto engine = std::make_unique<rt_audio::Project2>(rt_audio::StreamConfig{rate, 1, 128}, processor);
        ObservedAnalysis analysis;
        rt_audio::AnalysisWorker worker(*engine, analysis);
        worker.start();
        std::uint64_t blocks = 0;
        for (std::size_t offset = 0; offset < input.size(); offset += 128) {
            const auto frames = static_cast<std::uint32_t>(std::min<std::size_t>(128, input.size() - offset));
            const double time = static_cast<double>(offset) / rate;
            if (!engine->process_callback(input.data() + offset, output.data() + offset,
                                          frames, {time, time, 0})) {
                throw std::runtime_error("Project 2 rejected the input block");
            }
            ++blocks;
            // Offline pacing only: observe each analysis block before feeding the next.
            // A hardware audio callback must never perform this wait.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (analysis.completed.load(std::memory_order_acquire) < blocks) {
                if (worker.failed()) { worker.stop(); worker.rethrow_if_failed(); }
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("Analysis worker timed out");
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        worker.stop();
        worker.rethrow_if_failed();
        std::ofstream file(argv[2], std::ios::binary);
        file.write(reinterpret_cast<const char*>(output.data()),
                   static_cast<std::streamsize>(output.size() * sizeof(float)));
        file.close();
        if (!file) throw std::runtime_error("Cannot write output audio");

        const auto stats = engine->statistics();
        std::cout << std::setprecision(9)
                  << "{\"frames\":" << input.size() << ",\"callbacks\":" << stats.callbacks
                  << ",\"analysis_blocks\":" << analysis.blocks << ",\"analysis_frames\":" << analysis.frames
                  << ",\"analysis_rms\":" << std::sqrt(analysis.squares / static_cast<double>(analysis.frames))
                  << ",\"commands_submitted\":" << analysis.submitted
                  << ",\"commands_applied\":" << stats.commands_applied
                  << ",\"analysis_dropped\":" << stats.analysis_dropped
                  << ",\"commands_dropped\":" << stats.commands_dropped
                  << ",\"applied_revision\":" << processor.applied.revision << ",\"parameter\":";
        if (processor.applied.revision != 0) {
            const auto& n = processor.applied.notches[0];
            std::cout << "{\"frequency_hz\":" << n.frequency_hz << ",\"bandwidth_hz\":" << n.bandwidth_hz
                      << ",\"depth_db\":" << n.depth_db << ",\"transition_ms\":" << processor.applied.transition_ms << '}';
        } else { std::cout << "null"; }
        const auto count = std::min<std::size_t>(4, input.size());
        std::cout << ",\"input_head\":"; print_head(input.data(), count);
        std::cout << ",\"analysis_head\":"; print_head(analysis.head.data(), analysis.head_count);
        std::cout << ",\"output_head\":"; print_head(output.data(), count);
        std::cout << "}\n";
    } catch (const std::exception& error) {
        std::cerr << "project2 notebook: " << error.what() << '\n';
        return 1;
    }
}
