// SPDX-License-Identifier: BSD-3-Clause
#include "rules/worker.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace budyk {

Worker::Worker(const char* name, size_t capacity)
    : name_(name != nullptr ? name : "worker"),
      capacity_(capacity > 0 ? capacity : 1) {}

Worker::~Worker() { stop(kDefaultGraceMs); }

bool Worker::post(Job job) {
    std::lock_guard<std::mutex> g(mtx_);
    if (stopping_ || queue_.size() >= capacity_) {
        ++dropped_;
        // Once when it starts happening, then sparsely: a channel that
        // is down for an hour would otherwise log once per tick.
        if (dropped_ == 1 || dropped_ % 100 == 0) {
            std::fprintf(stderr, "budyk %s: %s, dropping job (%llu dropped so far)\n",
                         name_, stopping_ ? "stopped" : "queue full",
                         static_cast<unsigned long long>(dropped_));
        }
        return false;
    }
    queue_.push_back(std::move(job));
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    cv_.notify_one();
    return true;
}

bool Worker::wait_idle(int timeout_ms) {
    std::unique_lock<std::mutex> lk(mtx_);
    return idle_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0),
                             [this] { return queue_.empty() && !busy_; });
}

void Worker::stop(int grace_ms) {
    {
        std::lock_guard<std::mutex> g(mtx_);
        if (!thread_.joinable()) {               // never started: nothing to drain
            stopping_ = true;
            return;
        }
        if (stopping_) return;
    }
    wait_idle(grace_ms);
    size_t left = 0;
    {
        std::lock_guard<std::mutex> g(mtx_);
        stopping_ = true;
        left = queue_.size() + (busy_ ? 1 : 0);
        if (left > 0) cancel_.store(true);
    }
    cv_.notify_all();
    thread_.join();
    if (left > 0) {
        std::fprintf(stderr, "budyk %s: stopped with %zu job(s) unfinished\n", name_, left);
    }
}

size_t Worker::pending() const {
    std::lock_guard<std::mutex> g(mtx_);
    return queue_.size();
}

uint64_t Worker::completed() const {
    std::lock_guard<std::mutex> g(mtx_);
    return completed_;
}

uint64_t Worker::dropped() const {
    std::lock_guard<std::mutex> g(mtx_);
    return dropped_;
}

const char* Worker::name() const { return name_; }

void Worker::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return !queue_.empty() || stopping_; });
            if (stopping_ && (queue_.empty() || cancel_.load())) {
                // Cancelled: what is still queued will not run.
                dropped_ += queue_.size();
                queue_.clear();
                idle_cv_.notify_all();
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        job(cancel_);
        {
            std::lock_guard<std::mutex> g(mtx_);
            busy_ = false;
            ++completed_;
        }
        idle_cv_.notify_all();
    }
}

} // namespace budyk
