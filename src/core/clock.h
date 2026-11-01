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

} // namespace budyk
