#pragma once

#include <rt_audio/project2/analysis.hpp>

#include <atomic>
#include <exception>
#include <thread>

namespace rt_audio {

template <class Protocol>
class Engine;

template <class Protocol>
class Worker final {
public:
    using Handler = typename Protocol::Handler;
    Worker(Engine<Protocol>& engine, Handler& handler) noexcept;
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    // Control-thread lifecycle only. One start per worker instance.
    void start();
    void stop() noexcept;
    [[nodiscard]] bool failed() const noexcept;
    // Call after stop(); propagates detector errors outside the audio callback.
    void rethrow_if_failed() const;

private:
    void run() noexcept;
    Engine<Protocol>& engine_;
    Handler& handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    bool started_ = false;
    std::thread thread_;
    std::exception_ptr failure_;
};

struct DefaultProtocol;
// Definitions live in detail/worker_impl.hpp; worker.cpp instantiates this one.
using AnalysisWorker = Worker<DefaultProtocol>;
extern template class Worker<DefaultProtocol>;

} // namespace rt_audio
