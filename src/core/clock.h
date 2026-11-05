// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <ctime>

namespace budyk {

/// Wall-clock time in nanoseconds since the epoch: the timestamp every
/// sample carries and that the HTTP range queries are expressed in.
inline uint64_t now_realtime_ns() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

/// A clock that a step of the wall clock does not move, in nanoseconds
/// from an arbitrary point: for windows and timeouts measured from now
/// (the login throttle's), not for timestamps.
inline uint64_t now_monotonic_ns() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

/// The sleep that ends `interval_ms` after a tick that started
/// `elapsed_ns` ago: the interval less the tick's own length, never
/// below 0. Sleeping the whole interval after the tick put the tick's
/// length on every period (a 50 ms tick made L3 0.95 Hz, a 100 ms
/// level 6.7 Hz, and the rates were over a period that much longer).
inline int sleep_left_ms(int interval_ms, uint64_t elapsed_ns) {
    if (interval_ms <= 0) return 0;
    const uint64_t elapsed_ms = elapsed_ns / 1000000ULL;
    if (elapsed_ms >= static_cast<uint64_t>(interval_ms)) return 0;
    return interval_ms - static_cast<int>(elapsed_ms);
}

} // namespace budyk
