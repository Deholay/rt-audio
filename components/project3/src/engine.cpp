#include <rt_audio/project3/engine.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rt_audio::project3 {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

Engine::Engine(StreamConfig config) : config_(config) {
    if (!std::isfinite(config.sample_rate) || config.sample_rate < 8000.0 ||
        config.sample_rate > 384000.0 || config.channels == 0 || config.channels > max_channels ||
        config.frames_per_buffer == 0 || config.frames_per_buffer > max_frames ||
        config.block_frames == 0 || config.block_frames > max_frames ||
        config.delay_frames > max_delay_frames || config.lookahead_frames > max_delay_frames ||
        config.delay_frames < config.block_frames + config.lookahead_frames) {
        throw std::invalid_argument("Invalid Project 3 format/delay: delay must cover one hop plus lookahead");
    }
}

bool Engine::process_callback(const float* input, float* output,
                              std::uint32_t frames, CallbackInfo info) noexcept {
    // Leave enough headroom for block-end arithmetic in the last possible hop.
    if (output == nullptr || frames == 0 || frames > max_frames ||
        input_cursor_ > std::numeric_limits<std::uint64_t>::max() - frames - max_frames) {
        invalid_callbacks_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    if (input == nullptr) input_missing_.fetch_add(1, std::memory_order_relaxed);
    if ((info.status_flags & InputOverflow) != 0) input_overflows_.fetch_add(1, std::memory_order_relaxed);
    if ((info.status_flags & OutputUnderflow) != 0) output_underflows_.fetch_add(1, std::memory_order_relaxed);
    capture(input, frames, info);
    render(output, frames);
    captured_frames_.fetch_add(frames, std::memory_order_relaxed);
    rendered_frames_.fetch_add(frames, std::memory_order_relaxed);
    return true;
}

void Engine::capture(const float* input, std::uint32_t frames, CallbackInfo info) noexcept {
    constexpr auto dry_capacity = max_delay_frames + max_frames;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        if (building_frames_ == 0) {
            building_.first_sample = input_cursor_;
            building_.input_time_seconds = info.input_time_seconds + frame / config_.sample_rate;
            building_.frames = config_.block_frames;
            building_.channels = config_.channels;
            building_.status_flags = 0;
            building_.input_missing = 0;
        }
        building_.status_flags |= info.status_flags;
        if (input == nullptr) building_.input_missing = 1;
        const auto dry_offset = (input_cursor_ % dry_capacity) * config_.channels;
        for (std::uint32_t channel = 0; channel < config_.channels; ++channel) {
            const float sample = input ? input[frame * config_.channels + channel] : 0.0F;
            dry_[dry_offset + channel] = sample;
            building_.samples[building_frames_ * config_.channels + channel] = sample;
        }
        ++input_cursor_;
        if (++building_frames_ == config_.block_frames) {
            // Publish bounds BEFORE the input queue makes the block visible.
            published_input_end_.store(input_cursor_, std::memory_order_release);
            if (!input_queue_.try_push(building_)) {
                input_dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            building_frames_ = 0;
        }
    }
}

void Engine::select_block(std::uint64_t first_sample, std::size_t& pop_budget) noexcept {
    // Commit the whole source block, even if this callback renders only part.
    // A late result must never replace dry audio halfway through that block.
    committed_output_end_.store(first_sample + config_.block_frames, std::memory_order_release);
    use_processed_ = false;
    for (;;) {
        if (!have_pending_) {
            if (pop_budget == 0) break;
            --pop_budget;
            if (!output_queue_.try_pop(pending_)) break;
            have_pending_ = true;
        }
        if (pending_.first_sample < first_sample) {
            have_pending_ = false;
            output_stale_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        use_processed_ = pending_.first_sample == first_sample;
        break; // Future block remains cached until its exact playback position.
    }
    if (use_processed_) processed_blocks_.fetch_add(1, std::memory_order_relaxed);
    else fallback_blocks_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::render(float* output, std::uint32_t frames) noexcept {
    constexpr auto dry_capacity = max_delay_frames + max_frames;
    std::size_t pop_budget = max_output_pops_per_callback;
    std::uint64_t startup = 0, processed = 0, fallback = 0;
    for (std::uint32_t frame = 0; frame < frames; ++frame, ++output_cursor_) {
        if (output_cursor_ < config_.delay_frames) {
            std::fill_n(output + frame * config_.channels, config_.channels, 0.0F);
            ++startup;
            continue;
        }
        const auto source = output_cursor_ - config_.delay_frames;
        const auto offset = source % config_.block_frames;
        if (offset == 0) select_block(source, pop_budget);
        const float* samples = use_processed_
            ? pending_.samples.data() + offset * config_.channels
            : dry_.data() + (source % dry_capacity) * config_.channels;
        std::copy_n(samples, config_.channels, output + frame * config_.channels);
        if (use_processed_) ++processed;
        else ++fallback;
        if (offset + 1 == config_.block_frames && use_processed_) have_pending_ = false;
    }
    startup_frames_.fetch_add(startup, std::memory_order_relaxed);
    processed_frames_.fetch_add(processed, std::memory_order_relaxed);
    fallback_frames_.fetch_add(fallback, std::memory_order_relaxed);
}

bool Engine::try_read_input(AudioBlock& block) noexcept { return input_queue_.try_pop(block); }

bool Engine::try_submit_output(const AudioBlock& block) noexcept {
    if (block.frames != config_.block_frames || block.channels != config_.channels ||
        block.first_sample % config_.block_frames != 0 ||
        block.first_sample < last_submitted_end_ ||
        block.first_sample > std::numeric_limits<std::uint64_t>::max() - config_.block_frames ||
        block.first_sample + config_.block_frames > published_input_end_.load(std::memory_order_acquire)) {
        output_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (block.first_sample < playback_deadline()) {
        output_late_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!output_queue_.try_push(block)) {
        output_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    last_submitted_end_ = block.first_sample + block.frames;
    return true;
}

std::uint64_t Engine::playback_deadline() const noexcept {
    return committed_output_end_.load(std::memory_order_acquire);
}

Statistics Engine::statistics() const noexcept {
    return {callbacks_.load(std::memory_order_relaxed), captured_frames_.load(std::memory_order_relaxed),
        rendered_frames_.load(std::memory_order_relaxed), startup_frames_.load(std::memory_order_relaxed),
        processed_frames_.load(std::memory_order_relaxed), fallback_frames_.load(std::memory_order_relaxed),
        processed_blocks_.load(std::memory_order_relaxed), fallback_blocks_.load(std::memory_order_relaxed),
        input_dropped_.load(std::memory_order_relaxed), output_dropped_.load(std::memory_order_relaxed),
        output_late_.load(std::memory_order_relaxed), output_stale_.load(std::memory_order_relaxed),
        output_rejected_.load(std::memory_order_relaxed), input_missing_.load(std::memory_order_relaxed),
        invalid_callbacks_.load(std::memory_order_relaxed), input_overflows_.load(std::memory_order_relaxed),
        output_underflows_.load(std::memory_order_relaxed)};
}

} // namespace rt_audio::project3
