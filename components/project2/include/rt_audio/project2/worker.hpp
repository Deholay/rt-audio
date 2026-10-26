#pragma once

#include <rt_audio/project2/analysis.hpp>

#include <atomic>
#include <exception>
#include <thread>

namespace rt_audio {

class Project2;

class AnalysisWorker final {
public:
    AnalysisWorker(Project2& engine, AnalysisHandler& handler) noexcept;
    ~AnalysisWorker();
    AnalysisWorker(const AnalysisWorker&) = delete;
    AnalysisWorker& operator=(const AnalysisWorker&) = delete;
    // Control-thread lifecycle only. One start per worker instance.
    void start();
    void stop() noexcept;
    [[nodiscard]] bool failed() const noexcept;
    // Call after stop(); propagates detector errors outside the audio callback.
    void rethrow_if_failed() const;

private:
    void run() noexcept;
    Project2& engine_;
    AnalysisHandler& handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    bool started_ = false;
    std::thread thread_;
    std::exception_ptr failure_;
};

} // namespace rt_audio
