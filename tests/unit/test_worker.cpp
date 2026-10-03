// SPDX-License-Identifier: BSD-3-Clause
#include "rules/worker.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace budyk;

static double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int main() {
    // 1. Jobs run in order on the worker's thread, after post() returned;
    //    wait_idle() sees them through; counters add up.
    {
        Worker w("t1", 8);
        assert(w.pending() == 0 && w.completed() == 0 && w.dropped() == 0);

        std::atomic<int> order{0};
        int seen[3] = {-1, -1, -1};
        const std::thread::id caller = std::this_thread::get_id();
        std::atomic<bool> on_other_thread{false};
        for (int i = 0; i < 3; ++i) {
            assert(w.post([&, i](const std::atomic<bool>&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                seen[order++] = i;
                if (std::this_thread::get_id() != caller) on_other_thread = true;
            }));
        }
        assert(w.wait_idle(5000));
        assert(order == 3);
        assert(seen[0] == 0 && seen[1] == 1 && seen[2] == 2);
        assert(on_other_thread);
        assert(w.completed() == 3 && w.pending() == 0 && w.dropped() == 0);
    }

    // 2. A full queue refuses the job and counts it; the queued ones
    //    still run once the blocker finishes.
    {
        Worker w("t2", 2);
        std::atomic<bool> release{false};
        std::atomic<int>  ran{0};
        assert(w.post([&](const std::atomic<bool>&) {       // runs now, blocks
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++ran;
        }));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));   // it is busy
        assert(w.post([&](const std::atomic<bool>&) { ++ran; }));    // queued 1
        assert(w.post([&](const std::atomic<bool>&) { ++ran; }));    // queued 2
        assert(!w.post([&](const std::atomic<bool>&) { ++ran; }));   // full
        assert(w.pending() == 2 && w.dropped() == 1);
        assert(!w.wait_idle(50));                           // still blocked
        release = true;
        assert(w.wait_idle(5000));
        assert(ran == 3 && w.completed() == 3 && w.dropped() == 1);
    }

    // 3. stop(): a job that honours the cancel flag returns at once
    //    after the grace period, what is still queued is dropped, and
    //    post() fails afterwards. A worker that never ran a job stops
    //    without starting a thread.
    {
        Worker w("t3", 8);
        std::atomic<bool> cancelled{false};
        assert(w.post([&](const std::atomic<bool>& cancel) {
            const auto t0 = std::chrono::steady_clock::now();
            while (!cancel && seconds_since(t0) < 10) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            cancelled = cancel.load();
        }));
        assert(w.post([&](const std::atomic<bool>&) {}));   // never runs
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto t0 = std::chrono::steady_clock::now();
        w.stop(100);
        assert(seconds_since(t0) < 2.0);
        assert(cancelled);
        assert(w.completed() == 1 && w.dropped() == 1 && w.pending() == 0);
        assert(!w.post([&](const std::atomic<bool>&) {}));
        assert(w.dropped() == 2);
        w.stop(100);                                        // idempotent

        Worker idle("t3b");
        idle.stop(100);
        assert(!idle.post([&](const std::atomic<bool>&) {}));
    }

    // 4. stop() with a grace period lets a short job finish normally.
    {
        Worker w("t4", 8);
        std::atomic<int> ran{0};
        assert(w.post([&](const std::atomic<bool>&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ++ran;
        }));
        w.stop(2000);
        assert(ran == 1 && w.completed() == 1 && w.dropped() == 0);
    }

    std::printf("test_worker: PASS\n");
    return 0;
}
