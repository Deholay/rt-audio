#pragma once

// Template definitions for Worker<Protocol>. Include only where a protocol is
// instantiated (worker.cpp for DefaultProtocol, a release adapter for others).
#include <rt_audio/project2/engine.hpp>
#include <rt_audio/project2/worker.hpp>

#include <chrono>
#include <stdexcept>

namespace rt_audio {

template <class Protocol>
Worker<Protocol>::Worker(Engine<Protocol>& engine, Handler& handler) noexcept
    : engine_(engine), handler_(handler) {}

template <class Protocol>
Worker<Protocol>::~Worker() { stop(); }

template <class Protocol>
void Worker<Protocol>::start() {
    if (started_) throw std::logic_error("Worker is single-use");
    handler_.prepare(engine_.config());
    running_.store(true, std::memory_order_relaxed);
    try {
        thread_ = std::thread(&Worker::run, this);
        started_ = true;
    } catch (...) {
        running_.store(false, std::memory_order_relaxed);
        throw;
    }
}

template <class Protocol>
void Worker<Protocol>::stop() noexcept {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

template <class Protocol>
bool Worker<Protocol>::failed() const noexcept {
    return failed_.load(std::memory_order_acquire);
}

template <class Protocol>
void Worker<Protocol>::rethrow_if_failed() const {
    if (thread_.joinable()) throw std::logic_error("Stop worker before reading failure");
    if (failure_) std::rethrow_exception(failure_);
}

template <class Protocol>
void Worker<Protocol>::run() noexcept {
    try {
        typename Protocol::Tap tap{};
        while (running_.load(std::memory_order_relaxed)) {
            if (engine_.try_read_analysis(tap)) {
                Protocol::deliver(handler_, tap, engine_);
            } else {
                // Only the background worker sleeps; the audio thread never waits.
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    } catch (...) {
        failure_ = std::current_exception();
        failed_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_relaxed);
    }
}

} // namespace rt_audio
