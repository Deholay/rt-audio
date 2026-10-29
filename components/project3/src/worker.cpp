#include <rt_audio/project3/worker.hpp>

#include <chrono>
#include <stdexcept>

namespace rt_audio::project3 {

ProcessingWorker::ProcessingWorker(Engine& engine, AudioProcessor& processor) noexcept
    : engine_(engine), processor_(processor) {}

ProcessingWorker::~ProcessingWorker() { stop(); }

void ProcessingWorker::start() {
    if (started_) throw std::logic_error("ProcessingWorker is single-use");
    processor_.prepare(engine_.config());
    running_.store(true, std::memory_order_relaxed);
    try {
        thread_ = std::thread(&ProcessingWorker::run, this);
        started_ = true;
    } catch (...) {
        running_.store(false, std::memory_order_relaxed);
        throw;
    }
}

void ProcessingWorker::stop() noexcept {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

bool ProcessingWorker::failed() const noexcept { return failed_.load(std::memory_order_acquire); }

void ProcessingWorker::rethrow_if_failed() const {
    if (thread_.joinable()) throw std::logic_error("Stop worker before reading failure");
    if (failure_) std::rethrow_exception(failure_);
}

void ProcessingWorker::run() noexcept {
    try {
        AudioBlock block{};
        while (running_.load(std::memory_order_relaxed)) {
            if (engine_.try_read_input(block)) processor_.on_audio(block, engine_);
            else std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    } catch (...) {
        failure_ = std::current_exception();
        failed_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_relaxed);
    }
}

} // namespace rt_audio::project3
