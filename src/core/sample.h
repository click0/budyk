// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>

namespace budyk {

enum class Level : uint8_t { L1 = 1, L2 = 2, L3 = 3 };

struct CpuStats   { double total_percent; uint32_t count; };
struct MemStats   { uint64_t total; uint64_t available; double available_percent; };
struct SwapStats  { uint64_t total; uint64_t used; double used_percent; };
struct LoadStats  { double avg_1m, avg_5m, avg_15m; };

struct Sample {
    uint64_t timestamp_nanos;
    Level    level;
    CpuStats  cpu;
    MemStats  mem;
    SwapStats swap;
    LoadStats load;
    double    uptime_seconds;
    // TODO: net[], disk[], processes, thermal, entropy, self
};

} // namespace budyk
