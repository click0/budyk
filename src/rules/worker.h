// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace budyk {

// One background thread with a bounded FIFO of jobs. The collector tick
// must never wait on the outside world, but a rule's action does: an
// alert channel is a curl run of up to 20 s, an exec() is a child that
// may run for its whole timeout. The tick posts such work here and
// carries on; the job runs on this thread, in order, one at a time.
//
// The queue is bounded so a channel that is down cannot pile up jobs
// without limit: when it is full, post() returns false and the job is
// dropped and counted. Jobs get a cancel flag; stop() waits a grace
// period for the queue to run dry, then raises the flag so a job that
// honours it (exec_command does: it kills the child) returns at once,
// and drops whatever is still queued. The thread starts on the first
// post(), so a daemon with no alert channels and no exec() has no
// extra thread.
class Worker {
public:
    using Job = std::function<void(const std::atomic<bool>& cancel)>;

    static constexpr int kDefaultGraceMs = 2000;

    explicit Worker(const char* name, size_t capacity = 256);
    ~Worker();                                   // stop(kDefaultGraceMs)
    Worker(const Worker&)            = delete;
    Worker& operator=(const Worker&) = delete;

    // Queue a job. False when the queue is full or the worker has been
    // stopped; the job is dropped either way (see dropped()).
    bool     post(Job job);

    // Block until every queued job has finished, or timeout_ms passed.
    // True when idle. For tests and for an orderly shutdown.
    bool     wait_idle(int timeout_ms);

    // Wait up to grace_ms for the queue to run dry, then cancel whatever
    // is still running, drop whatever is still queued, and join the
    // thread. Idempotent; post() fails afterwards.
    void     stop(int grace_ms);

    size_t   pending()   const;                  // queued, not yet started
    uint64_t completed() const;                  // jobs that ran to return
    uint64_t dropped()   const;                  // refused by post() or by stop()
    const char* name()   const;

private:
    void run();

    const char*                   name_;
    const size_t                  capacity_;
    mutable std::mutex            mtx_;
    std::condition_variable       cv_;           // queue or stopping changed
    std::condition_variable       idle_cv_;      // a job finished
    std::deque<Job>               queue_;
    bool                          busy_     = false;
    bool                          stopping_ = false;
    std::thread                   thread_;
    std::atomic<bool>             cancel_{false};
    uint64_t                      completed_ = 0;
    uint64_t                      dropped_   = 0;
};

} // namespace budyk
