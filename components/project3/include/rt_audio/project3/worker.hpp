#pragma once

#include <rt_audio/project3/engine.hpp>

#include <exception>
#include <thread>

namespace rt_audio::project3 {

// Single-use worker, control-thread lifecycle. Engine and processor must outlive
// it. Stop/destroy the device BEFORE stopping/destroying the worker or engine.
class ProcessingWorker final {
public:
    ProcessingWorker(Engine& engine, AudioProcessor& processor) noexcept;
    ~ProcessingWorker();
    ProcessingWorker(const ProcessingWorker&) = delete;
    ProcessingWorker& operator=(const ProcessingWorker&) = delete;
    void start();
    void stop() noexcept;
    [[nodiscard]] bool failed() const noexcept;
    void rethrow_if_failed() const; // after stop()

private:
    void run() noexcept;
    Engine& engine_;
    AudioProcessor& processor_;
    std::atomic<bool> running_{false}, failed_{false};
    bool started_ = false;
    std::thread thread_;
    std::exception_ptr failure_;
};

} // namespace rt_audio::project3
