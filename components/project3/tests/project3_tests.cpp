#include <rt_audio/project3.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace p3 = rt_audio::project3;

namespace {
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ": " #condition " at " + std::to_string(__LINE__)); } while (false)

template <class Exception, class Function>
void expect_throw(Function function) {
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
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

float sample(std::uint64_t frame, std::uint32_t channel = 0) {
    return (static_cast<float>((frame * 7 + channel * 101) % 997) - 498.0F) / 1024.0F;
}

p3::StreamConfig config(std::uint32_t hop = 4, std::uint32_t delay = 12,
                        std::uint32_t channels = 1) {
    return {48000.0, channels, hop, hop, 0, delay};
}

struct Host {
    explicit Host(p3::StreamConfig c) : engine(std::make_unique<p3::Engine>(c)) {}
    std::unique_ptr<p3::Engine> engine;
    std::uint64_t cursor = 0;
    std::array<float, p3::max_frames * p3::max_channels + 1> input{}, output{};

    void tick(std::uint32_t frames, bool in_place = false, bool missing = false) {
        const auto channels = engine->config().channels;
        for (std::uint32_t i = 0; i < frames; ++i) {
            for (std::uint32_t ch = 0; ch < channels; ++ch) input[i * channels + ch] = sample(cursor + i, ch);
        }
        output.fill(-42.0F);
        input[frames * channels] = -42.0F;
        float* destination = in_place ? input.data() : output.data();
        CHECK(engine->process_callback(missing ? nullptr : input.data(), destination, frames,
                                        {2.0 + cursor / 48000.0, 3.0 + cursor / 48000.0, 0}));
        CHECK(destination[frames * channels] == -42.0F);
        if (in_place) std::copy_n(input.begin(), frames * channels, output.begin());
        cursor += frames;
    }

    void expect_dry(std::uint32_t frames, float gain = 1.0F) const {
        const auto& c = engine->config();
        const auto begin = cursor - frames;
        for (std::uint32_t i = 0; i < frames; ++i) {
            for (std::uint32_t ch = 0; ch < c.channels; ++ch) {
                const auto t = begin + i;
                const auto expected = t < c.delay_frames ? 0.0F : gain * sample(t - c.delay_frames, ch);
                CHECK(output[i * c.channels + ch] == expected);
            }
        }
    }

    p3::AudioBlock pop() {
        p3::AudioBlock block;
        CHECK(engine->try_read_input(block));
        return block;
    }
};

void test_config_and_bounds() {
    auto c = config();
    const auto reject = [](p3::StreamConfig value) {
        expect_throw<std::invalid_argument>([&] { auto engine = std::make_unique<p3::Engine>(value); });
    };
    c.sample_rate = std::numeric_limits<double>::quiet_NaN(); reject(c);
    c = config(); c.sample_rate = 7999; reject(c);
    c = config(); c.sample_rate = 384001; reject(c);
    c = config(); c.channels = 0; reject(c);
    c = config(); c.channels = 3; reject(c);
    c = config(); c.block_frames = 0; reject(c);
    c = config(); c.block_frames = p3::max_frames + 1; reject(c);
    c = config(); c.frames_per_buffer = 0; reject(c);
    c = config(); c.frames_per_buffer = p3::max_frames + 1; reject(c);
    c = config(); c.delay_frames = 3; reject(c);
    c = config(); c.lookahead_frames = 9; reject(c);
    c = config(); c.lookahead_frames = std::numeric_limits<std::uint32_t>::max(); reject(c);
    c = config(); c.delay_frames = p3::max_delay_frames + 1; reject(c);
    Host h(config());
    float sentinel = 123;
    CHECK(!h.engine->process_callback(nullptr, &sentinel, 0));
    CHECK(!h.engine->process_callback(nullptr, &sentinel, p3::max_frames + 1));
    CHECK(!h.engine->process_callback(nullptr, nullptr, 1));
    CHECK(sentinel == 123);
    CHECK(h.engine->statistics().invalid_callbacks == 3);
    CHECK(h.engine->statistics().captured_frames == 0);
}

void test_variable_callbacks_dry_alignment_and_ring_wrap() {
    for (auto delay : {11U, p3::max_delay_frames}) {
        Host h(config(4, delay, 2));
        constexpr std::array sizes{1U, 17U, 3U, 128U, p3::max_frames, 5U};
        for (std::uint32_t n = 0; n < 120; ++n) {
            auto size = sizes[n % sizes.size()];
            h.tick(size, (n % 2) != 0);
            h.expect_dry(size);
        }
        auto s = h.engine->statistics();
        CHECK(s.startup_frames == delay);
        CHECK(s.rendered_frames == h.cursor);
        CHECK(s.fallback_frames + s.startup_frames == h.cursor);
        CHECK(s.input_dropped > 0);
    }
}

void test_reblocking_metadata_and_missing_input() {
    Host h(config(4, 12, 2));
    h.tick(3);
    p3::AudioBlock block{};
    CHECK(!h.engine->try_read_input(block));
    h.tick(6);
    for (std::uint64_t first : {0U, 4U}) {
        block = h.pop();
        CHECK(block.first_sample == first && block.frames == 4 && block.channels == 2);
        CHECK(std::abs(block.input_time_seconds - (2.0 + first / 48000.0)) < 1e-12);
        for (std::uint32_t i = 0; i < 4; ++i)
            for (std::uint32_t ch = 0; ch < 2; ++ch) CHECK(block.samples[i * 2 + ch] == sample(first + i, ch));
    }
    CHECK(h.engine->process_callback(nullptr, h.output.data(), 3,
                                     {2.0 + 9.0 / 48000.0, 3.0, p3::InputOverflow | p3::OutputUnderflow}));
    block = h.pop();
    CHECK(block.first_sample == 8 && block.input_missing == 1);
    CHECK(block.samples[0] == sample(8) && block.samples[1] == sample(8, 1));
    CHECK(std::all_of(block.samples.begin() + 2, block.samples.begin() + 8, [](float f) { return f == 0; }));
    CHECK(block.status_flags == (p3::InputOverflow | p3::OutputUnderflow));
    const auto s = h.engine->statistics();
    CHECK(s.input_missing == 1 && s.input_overflows == 1 && s.output_underflows == 1);
    // All-null capture eventually plays silence on the dry route too.
    for (int n = 0; n < 8; ++n) {
        CHECK(h.engine->process_callback(nullptr, h.output.data(), 4));
        if (n >= 4) CHECK(std::all_of(h.output.begin(), h.output.begin() + 8, [](float f) { return f == 0; }));
    }
}

void amplify(p3::AudioBlock& block, float gain) {
    for (std::uint32_t i = 0; i < block.frames * block.channels; ++i) block.samples[i] *= gain;
}

void test_processed_audio_and_future_blocks() {
    Host h(config(4, 12, 2));
    for (int n = 0; n < 3; ++n) { h.tick(4); h.expect_dry(4); }
    (void)h.pop(); // deliberately omit source block 0
    auto a = h.pop(); auto b = h.pop();
    amplify(a, 2); amplify(b, 2);
    CHECK(h.engine->try_submit_output(a));
    CHECK(h.engine->try_submit_output(b));
    h.tick(4); h.expect_dry(4); // future output 4 must not be played at source 0
    h.tick(4, true); h.expect_dry(4, 2);
    h.tick(4); h.expect_dry(4, 2);
    h.tick(4); h.expect_dry(4); // exhausted worker returns to aligned dry
    const auto s = h.engine->statistics();
    CHECK(s.processed_frames == 8 && s.fallback_frames == 8);
    CHECK(s.processed_blocks == 2 && s.fallback_blocks == 2);
}

void test_late_results_never_replace_partial_block() {
    Host h(config(4, 5));
    h.tick(4);
    auto late = h.pop(); amplify(late, 2);
    h.tick(2); h.expect_dry(2); // source sample 0 played dry, rest still unrendered
    CHECK(h.engine->playback_deadline() == 4);
    CHECK(!h.engine->try_submit_output(late));
    CHECK(h.engine->statistics().output_late == 1);
    h.tick(3); h.expect_dry(3); // rest of same block remains dry
    auto next = h.pop(); amplify(next, 3);
    CHECK(h.engine->try_submit_output(next));
    h.tick(1); h.expect_dry(1, 3);
    h.tick(3); h.expect_dry(3, 3); // processed selection also persists across callbacks
    CHECK(h.engine->statistics().processed_frames == 4);
}

void test_input_overflow_keeps_dry_and_exposes_gap() {
    Host h(config());
    for (std::size_t n = 0; n < p3::queue_capacity + 2; ++n) { h.tick(4); h.expect_dry(4); }
    CHECK(h.engine->statistics().input_dropped == 2);
    for (std::size_t n = 0; n < p3::queue_capacity; ++n) CHECK(h.pop().first_sample == n * 4);
    h.tick(4); h.expect_dry(4);
    CHECK(h.pop().first_sample == (p3::queue_capacity + 2) * 4);
}

void test_output_validation_full_queue_and_retry() {
    Host h(config(4, 256));
    for (std::size_t n = 0; n < p3::queue_capacity; ++n) {
        h.tick(4); CHECK(h.engine->try_submit_output(h.pop()));
    }
    h.tick(4);
    auto pending = h.pop();
    CHECK(!h.engine->try_submit_output(pending));
    CHECK(h.engine->statistics().output_dropped == 1);
    auto bad = pending; bad.frames = 3; CHECK(!h.engine->try_submit_output(bad));
    bad = pending; bad.channels = 2; CHECK(!h.engine->try_submit_output(bad));
    bad = pending; ++bad.first_sample; CHECK(!h.engine->try_submit_output(bad));
    bad = pending; bad.first_sample = 0; CHECK(!h.engine->try_submit_output(bad));
    bad = pending; bad.first_sample = 1000; CHECK(!h.engine->try_submit_output(bad));
    bad = pending; bad.first_sample = std::numeric_limits<std::uint64_t>::max() - 3;
    CHECK(!h.engine->try_submit_output(bad));
    CHECK(h.engine->statistics().output_rejected == 6);
    while (h.cursor <= 256) { h.tick(4); h.expect_dry(4); }
    CHECK(h.engine->try_submit_output(pending)); // slot freed, original sequence retry allowed
    CHECK(!h.engine->try_submit_output(pending)); // duplicate now rejected
    for (int n = 0; n < 40; ++n) { h.tick(4); h.expect_dry(4); }
}

// A test-only transform with two future hops. Demonstrates I/O buffering, not
// a key-noise algorithm. Copies state because on_audio references are transient.
class LookaheadProcessor final : public p3::AudioProcessor {
public:
    void on_audio(const p3::AudioBlock& input, p3::OutputSink& output) override {
        if (count >= 2) {
            auto result = history[count % 2];
            amplify(result, 2);
            CHECK(output.try_submit_output(result));
        }
        history[count % 2] = input;
        ++count;
    }
    std::array<p3::AudioBlock, 2> history{};
    std::size_t count = 0;
};

void test_lookahead_plugin_preserves_source_time() {
    auto c = config(4, 16); c.lookahead_frames = 8;
    Host h(c);
    LookaheadProcessor processor;
    for (int n = 0; n < 40; ++n) {
        h.tick(4);
        h.expect_dry(4, 2);
        processor.on_audio(h.pop(), *h.engine);
    }
    CHECK(h.engine->statistics().fallback_frames == 0);
    CHECK(h.engine->statistics().processed_frames == 144);
}

void test_bounded_pop_budget_with_tiny_hops() {
    // At most 32 pop attempts are allowed even for a 2048-frame callback. Output
    // remains fully defined when the host spans far more hops than this budget.
    Host h(config(1, p3::max_frames));
    h.tick(p3::max_frames);
    for (std::size_t n = 0; n < p3::queue_capacity; ++n) {
        auto block = h.pop(); amplify(block, 2);
        CHECK(h.engine->try_submit_output(block));
    }
    h.tick(p3::max_frames);
    for (std::uint32_t n = 0; n < p3::max_frames; ++n) {
        CHECK(h.output[n] == sample(n) * (n < p3::queue_capacity ? 2.0F : 1.0F));
    }
    CHECK(h.engine->statistics().processed_blocks == p3::queue_capacity);
}

class CheckingProcessor final : public p3::AudioProcessor {
public:
    void prepare(const p3::StreamConfig& c) override { prepared = c.channels == 2; }
    void on_audio(const p3::AudioBlock& block, p3::OutputSink& output) override {
        if (block.first_sample < last_end || block.frames != 128 || block.channels != 2) valid = false;
        for (std::uint32_t i = 0; i < block.frames; ++i)
            for (std::uint32_t ch = 0; ch < 2; ++ch)
                if (block.samples[i * 2 + ch] != sample(block.first_sample + i, ch)) valid = false;
        last_end = block.first_sample + block.frames;
        (void)output.try_submit_output(block);
        calls.fetch_add(1, std::memory_order_release);
    }
    bool prepared = false, valid = true; // inspect after start / join respectively
    std::uint64_t last_end = 0;
    std::atomic<std::uint64_t> calls{0};
};

void test_threaded_worker_integrity_and_lifecycle() {
    Host h(config(128, 512, 2));
    CheckingProcessor processor;
    p3::ProcessingWorker worker(*h.engine, processor);
    worker.stop(); worker.start();
    CHECK(processor.prepared);
    expect_throw<std::logic_error>([&] { worker.start(); });
    expect_throw<std::logic_error>([&] { worker.rethrow_if_failed(); });
    h.tick(128); h.expect_dry(128);
    CHECK(wait_until([&] { return processor.calls.load(std::memory_order_acquire) > 0; }));
    for (int n = 0; n < 5000; ++n) { h.tick(128, n % 2 == 0); h.expect_dry(128); }
    worker.stop(); worker.stop(); worker.rethrow_if_failed();
    CHECK(!worker.failed() && processor.valid);
    CHECK(processor.calls.load() > 0);
    CHECK(h.engine->statistics().processed_frames > 0);
    expect_throw<std::logic_error>([&] { worker.start(); });
    for (int n = 0; n < 40; ++n) { h.tick(128); h.expect_dry(128); }
    const auto s = h.engine->statistics();
    CHECK(s.processed_frames + s.fallback_frames + s.startup_frames == s.rendered_frames);
}

class ThrowingProcessor final : public p3::AudioProcessor {
public:
    void on_audio(const p3::AudioBlock&, p3::OutputSink&) override { throw std::runtime_error("inference failed"); }
};

void test_failed_worker_keeps_aligned_fallback() {
    Host h(config());
    ThrowingProcessor processor;
    p3::ProcessingWorker worker(*h.engine, processor);
    worker.start(); h.tick(4);
    CHECK(wait_until([&] { return worker.failed(); }));
    for (int n = 0; n < 50; ++n) { h.tick(4); h.expect_dry(4); }
    worker.stop();
    expect_throw<std::runtime_error>([&] { worker.rethrow_if_failed(); });
    CHECK(h.engine->statistics().input_dropped > 0);
}

void test_queue_rollover_and_failed_pop() {
    p3::detail::SpscRing<std::uint64_t, 4> queue(std::numeric_limits<std::uint64_t>::max() - 1);
    std::uint64_t value = 42;
    CHECK(!queue.try_pop(value) && value == 42);
    for (std::uint64_t n = 0; n < 4; ++n) CHECK(queue.try_push(n));
    CHECK(!queue.try_push(99));
    for (std::uint64_t n = 0; n < 4; ++n) { CHECK(queue.try_pop(value)); CHECK(value == n); }
    CHECK(!queue.try_pop(value) && value == 3);
}

} // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"configuration and callback bounds", test_config_and_bounds},
        {"variable callbacks, stereo, in-place and dry ring wrap", test_variable_callbacks_dry_alignment_and_ring_wrap},
        {"reblocking, timestamps and missing input", test_reblocking_metadata_and_missing_input},
        {"processed output, future results and dry recovery", test_processed_audio_and_future_blocks},
        {"late output cannot replace a partially played block", test_late_results_never_replace_partial_block},
        {"input queue overflow keeps playback aligned", test_input_overflow_keeps_dry_and_exposes_gap},
        {"output validation, full queue and retry", test_output_validation_full_queue_and_retry},
        {"lookahead plugin retains original sample positions", test_lookahead_plugin_preserves_source_time},
        {"bounded output queue work with tiny hops", test_bounded_pop_budget_with_tiny_hops},
        {"threaded worker data integrity and lifecycle", test_threaded_worker_integrity_and_lifecycle},
        {"worker failure preserves aligned dry playback", test_failed_worker_keeps_aligned_fallback},
        {"SPSC cursor rollover and failed pop", test_queue_rollover_and_failed_pop},
    };
    try {
        for (const auto& t : tests) { t.run(); std::cout << "PASS " << t.name << '\n'; }
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
    std::cout << "All " << std::size(tests) << " Project 3 tests passed.\n";
}
