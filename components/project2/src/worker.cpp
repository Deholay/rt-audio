#include <rt_audio/project2/worker.hpp>
#include <rt_audio/project2/engine.hpp>

#include <chrono>
#include <stdexcept>

namespace rt_audio {

AnalysisWorker::AnalysisWorker(Project2& engine, AnalysisHandler& handler) noexcept
    : engine_(engine), handler_(handler) {}

AnalysisWorker::~AnalysisWorker() { stop(); }

void AnalysisWorker::start() {
    if (started_) throw std::logic_error("AnalysisWorker is single-use");
    handler_.prepare(engine_.config());
    running_.store(true, std::memory_order_relaxed);
    try {
        thread_ = std::thread(&AnalysisWorker::run, this);
        started_ = true;
    } catch (...) {
        running_.store(false, std::memory_order_relaxed);
        throw;
    }
}

void AnalysisWorker::stop() noexcept {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

bool AnalysisWorker::failed() const noexcept {
    return failed_.load(std::memory_order_acquire);
}

void AnalysisWorker::rethrow_if_failed() const {
    if (thread_.joinable()) throw std::logic_error("Stop worker before reading failure");
    if (failure_) std::rethrow_exception(failure_);
}

void AnalysisWorker::run() noexcept {
    try {
        AnalysisBlock block{};
        while (running_.load(std::memory_order_relaxed)) {
            if (engine_.try_read_analysis(block)) {
                handler_.on_audio(block, engine_);
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
