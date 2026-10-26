#include <rt_audio/project2.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

// Demonstrates the detector OUTPUT port with a fixed command. This is not a
// howling detector. Replace on_audio() with your own rolling-window analysis.
class ExampleControlSource final : public rt_audio::AnalysisHandler {
public:
    void on_audio(const rt_audio::AnalysisBlock&, rt_audio::ParameterSink& output) override {
        if (published_) return;
        rt_audio::ParameterSnapshot parameters{};
        parameters.revision = 1;
        parameters.notch_count = 1;
        parameters.notches[0] = {1000.0F, 100.0F, 0.0F};
        parameters.transition_ms = 5.0F;
        // Retrying on the next input block is background policy, never an RT spin.
        published_ = output.try_submit_parameters(parameters);
    }

private:
    bool published_ = false;
};

} // namespace

int main() {
    try {
        rt_audio::StreamConfig config{};
        rt_audio::PassthroughProcessor processor;
        auto engine = std::make_unique<rt_audio::Project2>(config, processor);
        ExampleControlSource detector;
        rt_audio::AnalysisWorker worker(*engine, detector);
        std::array<float, 128> input{}, output{};
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<float>(i) / 256.0F - 0.25F;
        }
        worker.start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        // Device-free host invokes the same callback entry point as PortAudio.
        for (std::uint32_t block = 0; block < 128 ||
             engine->statistics().commands_applied == 0; ++block) {
            if (std::chrono::steady_clock::now() > deadline) {
                throw std::runtime_error("Analysis worker did not publish in time");
            }
            const auto time = static_cast<double>(block) * 128.0 / config.sample_rate;
            if (!engine->process_callback(input.data(), output.data(), 128,
                                          {time, time, 0}) || input != output) {
                throw std::runtime_error("Passthrough callback changed samples");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Simulated device is stopped before joining the analysis worker.
        worker.stop();
        worker.rethrow_if_failed();
        const auto s = engine->statistics();
        std::cout << "PASS: synthetic input -> callback -> output; analysis -> SPSC "
                     "parameters -> callback\n"
                  << "callbacks=" << s.callbacks
                  << " frames=" << s.processed_frames
                  << " parameter_updates=" << s.commands_applied
                  << " analysis_dropped=" << s.analysis_dropped << '\n'
                  << "No microphone/speaker opened. No suppression algorithm installed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
