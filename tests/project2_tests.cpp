#include <rt_audio/project2.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw std::runtime_error(std::string(__func__) + ": " #condition +   \
                                     " (line " + std::to_string(__LINE__) + ")"); \
        }                                                                       \
    } while (false)

template <class Exception, class Function>
void expect_throw(Function function) {
    bool caught = false;
    try {
        function();
    } catch (const Exception&) {
        caught = true;
    }
    CHECK(caught);
}

template <class Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

class RecordingProcessor final : public rt_audio::RealtimeProcessor {
public:
    void prepare(const rt_audio::StreamConfig& config) override {
        prepared = true;
        prepared_config = config;
    }

    void apply_parameters(const rt_audio::ParameterSnapshot& parameters) noexcept override {
        if (apply_count < revisions.size()) revisions[apply_count] = parameters.revision;
        ++apply_count;
        current_revision = parameters.revision;
        latest = parameters;
    }

    void process(const rt_audio::ProcessBlock& block) noexcept override {
        if (process_count < revisions_at_process.size()) {
            revisions_at_process[process_count] = current_revision;
        }
        ++process_count;
        last_first_sample = block.first_sample;
        last_timing = block.timing;
        for (std::size_t i = 0; i < static_cast<std::size_t>(block.frames) * block.channels; ++i) {
            block.output[i] = block.input[i] * gain;
        }
    }

    bool prepared = false;
    float gain = 1.0F;
    std::size_t apply_count = 0;
    std::size_t process_count = 0;
    std::uint64_t current_revision = 0;
    std::uint64_t last_first_sample = 0;
    rt_audio::StreamConfig prepared_config{};
    rt_audio::CallbackInfo last_timing{};
    rt_audio::ParameterSnapshot latest{};
    std::array<std::uint64_t, 64> revisions{};
    std::array<std::uint64_t, 64> revisions_at_process{};
};

rt_audio::ParameterSnapshot parameters(std::uint64_t revision) {
    rt_audio::ParameterSnapshot result{};
    result.revision = revision;
    result.notch_count = 1;
    result.notches[0] = rt_audio::NotchTarget{2000.0F, 100.0F, 12.0F};
    return result;
}

void test_ring_boundaries_and_rollover() {
    quant::ipc::SpscRing<std::uint64_t, 4> queue(
        std::numeric_limits<std::uint64_t>::max() - 1);
    std::uint64_t output = 999;
    CHECK(!queue.try_pop(output));
    CHECK(output == 999);
    for (std::uint64_t i = 0; i < 4; ++i) CHECK(queue.try_push(i));
    CHECK(!queue.try_push(999));
    CHECK(queue.try_pop(output));
    CHECK(output == 0);
    CHECK(queue.try_push(4));
    for (std::uint64_t i = 1; i <= 4; ++i) {
        CHECK(queue.try_pop(output));
        CHECK(output == i);
    }
    CHECK(!queue.try_pop(output));
    CHECK(output == 4);
    for (std::uint64_t i = 0; i < 100; ++i) {
        CHECK(queue.try_push(i));
        CHECK(queue.try_pop(output));
        CHECK(output == i);
    }
}

void test_config_validation() {
    rt_audio::PassthroughProcessor processor;
    const auto invalid = [&processor](rt_audio::StreamConfig config) {
        expect_throw<std::invalid_argument>([&] {
            auto engine = std::make_unique<rt_audio::Project2>(config, processor);
        });
    };
    invalid({std::numeric_limits<double>::quiet_NaN(), 1, 128});
    invalid({std::numeric_limits<double>::infinity(), 1, 128});
    invalid({7999.0, 1, 128});
    invalid({384001.0, 1, 128});
    invalid({48000.0, 0, 128});
    invalid({48000.0, rt_audio::max_channels + 1, 128});
    invalid({48000.0, 1, 0});
    invalid({48000.0, 1, rt_audio::max_frames + 1});
}

void test_passthrough_and_callback_bounds() {
    rt_audio::PassthroughProcessor processor;
    auto engine = std::make_unique<rt_audio::Project2>(
        rt_audio::StreamConfig{48000.0, 2, 128}, processor);
    std::array<float, rt_audio::max_frames * 2 + 1> input{};
    std::array<float, rt_audio::max_frames * 2 + 1> output{};
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(i % 100) / 100.0F;
    rt_audio::AnalysisBlock block{};
    std::uint64_t next_sample = 0;
    for (const auto frames : {1U, 17U, 128U, rt_audio::max_frames}) {
        output.fill(-42.0F);
        const rt_audio::CallbackInfo timing{1.25, 1.5,
            rt_audio::InputOverflow | rt_audio::OutputUnderflow};
        CHECK(engine->process_callback(input.data(), output.data(), frames, timing));
        CHECK(std::equal(input.begin(), input.begin() + frames * 2, output.begin()));
        CHECK(output[frames * 2] == -42.0F);
        CHECK(engine->try_read_analysis(block));
        CHECK(block.frames == frames);
        CHECK(block.channels == 2);
        CHECK(block.first_sample == next_sample);
        CHECK(block.input_time_seconds == timing.input_time_seconds);
        CHECK(block.status_flags == timing.status_flags);
        CHECK(block.input_missing == 0);
        CHECK(std::equal(input.begin(), input.begin() + frames * 2, block.samples.begin()));
        CHECK(!engine->try_read_analysis(block));
        next_sample += frames;
    }
    output.fill(-42.0F);
    CHECK(engine->process_callback(nullptr, output.data(), 17));
    CHECK(std::all_of(output.begin(), output.begin() + 34, [](float value) { return value == 0.0F; }));
    CHECK(output[34] == -42.0F);
    CHECK(engine->try_read_analysis(block));
    CHECK(block.input_missing == 1);
    CHECK(block.first_sample == next_sample);
    CHECK(std::all_of(block.samples.begin(), block.samples.begin() + 34,
                      [](float value) { return value == 0.0F; }));
    next_sample += 17;
    output.fill(-42.0F);
    CHECK(!engine->process_callback(input.data(), nullptr, 128));
    CHECK(!engine->process_callback(input.data(), output.data(), 0));
    CHECK(!engine->process_callback(input.data(), output.data(), rt_audio::max_frames + 1));
    CHECK(std::all_of(output.begin(), output.end(), [](float value) { return value == -42.0F; }));
    CHECK(!engine->try_read_analysis(block));
    const auto stats = engine->statistics();
    CHECK(stats.callbacks == 5);
    CHECK(stats.processed_frames == next_sample);
    CHECK(stats.input_missing == 1);
    CHECK(stats.invalid_callbacks == 3);
    CHECK(stats.input_overflows == 4);
    CHECK(stats.output_underflows == 4);
}

void test_analysis_tap_precedes_in_place_dsp() {
    RecordingProcessor processor;
    processor.gain = 2.0F;
    auto engine = std::make_unique<rt_audio::Project2>(
        rt_audio::StreamConfig{44100.0, 2, 128}, processor);
    CHECK(processor.prepared);
    CHECK(processor.prepared_config.sample_rate == 44100.0);
    CHECK(processor.prepared_config.channels == 2);
    std::array<float, 8> audio{0.1F, -0.1F, 0.2F, -0.2F, 0.3F, -0.3F, 0.4F, -0.4F};
    const auto original = audio;
    const rt_audio::CallbackInfo timing{5.0, 5.1, rt_audio::PrimingOutput};
    CHECK(engine->process_callback(audio.data(), audio.data(), 4, timing));
    for (std::size_t i = 0; i < audio.size(); ++i) CHECK(audio[i] == original[i] * 2.0F);
    rt_audio::AnalysisBlock block{};
    CHECK(engine->try_read_analysis(block));
    CHECK(std::equal(original.begin(), original.end(), block.samples.begin()));
    CHECK(processor.last_first_sample == 0);
    CHECK(processor.last_timing.output_time_seconds == timing.output_time_seconds);
}

void test_analysis_overflow_preserves_audio_and_exposes_gap() {
    rt_audio::PassthroughProcessor processor;
    auto engine = std::make_unique<rt_audio::Project2>(rt_audio::StreamConfig{}, processor);
    std::array<float, 3> input{0.125F, 0.25F, -0.5F};
    std::array<float, 3> output{};
    for (std::size_t i = 0; i < rt_audio::analysis_capacity + 3; ++i) {
        CHECK(engine->process_callback(input.data(), output.data(), 3));
        CHECK(input == output);
    }
    CHECK(engine->statistics().analysis_dropped == 3);
    rt_audio::AnalysisBlock block{};
    for (std::size_t i = 0; i < rt_audio::analysis_capacity; ++i) {
        CHECK(engine->try_read_analysis(block));
        CHECK(block.first_sample == i * 3);
    }
    CHECK(!engine->try_read_analysis(block));
    const auto expected_if_contiguous = block.first_sample + block.frames;
    CHECK(engine->process_callback(input.data(), output.data(), 3));
    CHECK(input == output);
    CHECK(engine->try_read_analysis(block));
    CHECK(block.first_sample == (rt_audio::analysis_capacity + 3) * 3);
    CHECK(block.first_sample - expected_if_contiguous == 9);
}

void test_parameter_validation() {
    RecordingProcessor processor;
    auto engine = std::make_unique<rt_audio::Project2>(rt_audio::StreamConfig{}, processor);
    std::uint64_t rejected = 0;
    const auto reject = [&](const rt_audio::ParameterSnapshot& p) {
        CHECK(!engine->try_submit_parameters(p));
        ++rejected;
        CHECK(engine->statistics().commands_rejected == rejected);
    };
    auto p = parameters(0);
    reject(p);
    p = parameters(1); p.notch_count = rt_audio::max_notches + 1; reject(p);
    p = parameters(1); p.bypass = 2; reject(p);
    for (float value : {-1.0F, 1001.0F, std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity()}) {
        p = parameters(1); p.transition_ms = value; reject(p);
    }
    for (float value : {0.0F, -1.0F, 24000.0F, std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity()}) {
        p = parameters(1); p.notches[0].frequency_hz = value; reject(p);
        p = parameters(1); p.notches[0].bandwidth_hz = value; reject(p);
    }
    for (float value : {-1.0F, 121.0F, std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity()}) {
        p = parameters(1); p.notches[0].depth_db = value; reject(p);
    }
    CHECK(engine->try_submit_parameters(parameters(2)));
    reject(parameters(2));
    reject(parameters(1));
    p = parameters(3); p.notch_count = 0; p.bypass = 1; p.transition_ms = 0.0F;
    CHECK(engine->try_submit_parameters(p));
    CHECK(processor.apply_count == 0);
    float input = 0.25F;
    float output = 0.0F;
    CHECK(engine->process_callback(&input, &output, 1));
    CHECK(processor.apply_count == 1);
    CHECK(processor.current_revision == 3);
    CHECK(processor.latest.bypass == 1);
    CHECK(processor.latest.notch_count == 0);
    CHECK(processor.revisions_at_process[0] == 3);
}

void test_bounded_parameter_coalescing_and_full_queue_retry() {
    RecordingProcessor processor;
    auto engine = std::make_unique<rt_audio::Project2>(rt_audio::StreamConfig{}, processor);
    for (std::uint64_t revision = 1; revision <= rt_audio::command_capacity; ++revision) {
        CHECK(engine->try_submit_parameters(parameters(revision)));
    }
    const auto retry = parameters(rt_audio::command_capacity + 1);
    CHECK(!engine->try_submit_parameters(retry));
    CHECK(engine->statistics().commands_dropped == 1);
    CHECK(engine->statistics().commands_rejected == 0);
    CHECK(processor.apply_count == 0);
    float input = -0.25F;
    float output = 0.0F;
    CHECK(engine->process_callback(&input, &output, 1));
    CHECK(output == input);
    CHECK(processor.apply_count == 1);
    CHECK(processor.current_revision == rt_audio::max_commands_per_callback);
    CHECK(processor.revisions_at_process[0] == rt_audio::max_commands_per_callback);
    CHECK(engine->try_submit_parameters(retry));
    for (std::size_t i = 2; i <= rt_audio::command_capacity / rt_audio::max_commands_per_callback; ++i) {
        CHECK(engine->process_callback(&input, &output, 1));
        CHECK(processor.apply_count == i);
        CHECK(processor.current_revision == i * rt_audio::max_commands_per_callback);
        CHECK(processor.revisions_at_process[i - 1] == processor.current_revision);
    }
    CHECK(engine->process_callback(&input, &output, 1));
    CHECK(processor.current_revision == retry.revision);
    const auto applications = processor.apply_count;
    CHECK(engine->process_callback(&input, &output, 1));
    CHECK(processor.apply_count == applications);
    CHECK(engine->statistics().commands_applied == applications);
}

struct QueueMessage {
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, 8> payload{};
};

void test_concurrent_ring_payload_integrity() {
    quant::ipc::SpscRing<QueueMessage, 64> queue;
    constexpr std::uint64_t count = 100000;
    std::atomic<bool> aborted{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto expired = [&] {
        if (aborted.load(std::memory_order_relaxed)) return true;
        if (std::chrono::steady_clock::now() < deadline) return false;
        aborted.store(true, std::memory_order_relaxed);
        return true;
    };
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            QueueMessage message{};
            message.sequence = i;
            for (std::size_t j = 0; j < message.payload.size(); ++j) message.payload[j] = i * 17 + j;
            while (!queue.try_push(message)) {
                if (expired()) return;
                std::this_thread::yield();
            }
        }
    });
    bool valid = true;
    std::uint64_t received = 0;
    for (; received < count; ++received) {
        QueueMessage message{};
        while (!queue.try_pop(message)) {
            if (expired()) break;
            std::this_thread::yield();
        }
        if (aborted.load(std::memory_order_relaxed)) break;
        if (message.sequence != received) valid = false;
        for (std::size_t j = 0; j < message.payload.size(); ++j) {
            if (message.payload[j] != received * 17 + j) valid = false;
        }
    }
    producer.join();
    CHECK(!aborted.load());
    CHECK(valid);
    CHECK(received == count);
}

class PublishingHandler final : public rt_audio::AnalysisHandler {
public:
    void prepare(const rt_audio::StreamConfig& config) override {
        prepared = config.channels == 1;
    }
    void on_audio(const rt_audio::AnalysisBlock& block, rt_audio::ParameterSink& output) override {
        if (block.frames != 4 || block.channels != 1 || block.samples[0] != 0.25F) valid = false;
        if (calls.load(std::memory_order_relaxed) == 0) {
            submitted = output.try_submit_parameters(parameters(1));
        }
        calls.fetch_add(1, std::memory_order_release);
    }
    bool prepared = false; // inspected after start() synchronously prepares
    bool valid = true; // inspected after join
    bool submitted = false; // inspected after join
    std::atomic<std::size_t> calls{0};
};

void test_analysis_worker_pipeline_and_lifecycle() {
    RecordingProcessor processor;
    auto engine = std::make_unique<rt_audio::Project2>(rt_audio::StreamConfig{}, processor);
    PublishingHandler handler;
    rt_audio::AnalysisWorker worker(*engine, handler);
    worker.stop(); // safe before start
    worker.start();
    CHECK(handler.prepared);
    expect_throw<std::logic_error>([&] { worker.start(); });
    expect_throw<std::logic_error>([&] { worker.rethrow_if_failed(); });
    const std::array<float, 4> input{0.25F, 0.125F, -0.25F, -0.5F};
    std::array<float, 4> output{};
    CHECK(engine->process_callback(input.data(), output.data(), 4));
    CHECK(output == input);
    CHECK(wait_until([&] { return handler.calls.load(std::memory_order_acquire) >= 1; }));
    CHECK(engine->process_callback(input.data(), output.data(), 4));
    CHECK(output == input);
    CHECK(processor.current_revision == 1);
    CHECK(wait_until([&] { return handler.calls.load(std::memory_order_acquire) >= 2; }));
    worker.stop();
    worker.stop();
    CHECK(handler.valid);
    CHECK(handler.submitted);
    CHECK(!worker.failed());
    worker.rethrow_if_failed();
    expect_throw<std::logic_error>([&] { worker.start(); });
    CHECK(engine->process_callback(input.data(), output.data(), 4));
    CHECK(output == input);
    CHECK(processor.current_revision == 1);
}

class ThrowingHandler final : public rt_audio::AnalysisHandler {
public:
    void on_audio(const rt_audio::AnalysisBlock&, rt_audio::ParameterSink&) override {
        throw std::runtime_error("detector failure");
    }
};

void test_worker_failure_does_not_interrupt_audio() {
    rt_audio::PassthroughProcessor processor;
    auto engine = std::make_unique<rt_audio::Project2>(rt_audio::StreamConfig{}, processor);
    ThrowingHandler handler;
    rt_audio::AnalysisWorker worker(*engine, handler);
    worker.start();
    float input = 0.375F;
    float output = 0.0F;
    CHECK(engine->process_callback(&input, &output, 1));
    CHECK(wait_until([&] { return worker.failed(); }));
    // Audio keeps flowing while the failed worker object is still alive.
    for (std::size_t i = 0; i < rt_audio::analysis_capacity + 2; ++i) {
        output = 0.0F;
        CHECK(engine->process_callback(&input, &output, 1));
        CHECK(output == input);
    }
    CHECK(engine->statistics().analysis_dropped > 0);
    worker.stop();
    expect_throw<std::runtime_error>([&] { worker.rethrow_if_failed(); });
}

} // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"ring boundaries and cursor rollover", test_ring_boundaries_and_rollover},
        {"stream configuration validation", test_config_validation},
        {"passthrough and callback bounds", test_passthrough_and_callback_bounds},
        {"pre-DSP analysis tap with in-place audio", test_analysis_tap_precedes_in_place_dsp},
        {"analysis overflow preserves playback and reports gaps", test_analysis_overflow_preserves_audio_and_exposes_gap},
        {"parameter validation", test_parameter_validation},
        {"bounded parameter coalescing and retry", test_bounded_parameter_coalescing_and_full_queue_retry},
        {"100000 concurrent messages retain payload integrity", test_concurrent_ring_payload_integrity},
        {"analysis worker pipeline and lifecycle", test_analysis_worker_pipeline_and_lifecycle},
        {"worker failure preserves playback", test_worker_failure_does_not_interrupt_audio},
    };
    try {
        for (const auto& test : tests) {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "All " << std::size(tests) << " tests passed.\n";
    return 0;
}
