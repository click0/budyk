// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>

namespace budyk {

enum class Level : uint8_t { L1 = 1, L2 = 2, L3 = 3 };

struct CpuStats   { double total_percent; uint32_t count; };
struct MemStats   { uint64_t total; uint64_t available; double available_percent; };
struct SwapStats  { uint64_t total; uint64_t used; double used_percent; };
struct LoadStats  { double avg_1m, avg_5m, avg_15m; };

// Aggregate disk I/O across all whole block devices (partitions excluded).
struct DiskStats {
    uint64_t read_bytes_per_sec;
    uint64_t write_bytes_per_sec;
    uint32_t device_count;
};

// Aggregate network I/O across all non-loopback interfaces.
struct NetStats {
    uint64_t rx_bytes_per_sec;
    uint64_t tx_bytes_per_sec;
    uint32_t interface_count;
};

// Process state — L2 / watchful tier (spec §3.3.2).
struct ProcessStats {
    uint32_t total;
    uint32_t running;
};

// Entropy pool state — L3 / active tier (spec §3.3.3).
// On platforms without an entropy_avail surface (currently FreeBSD),
// available_bits stays at 0 and `present` is false.
struct EntropyStats {
    uint32_t available_bits;
    bool     present;
};

// Self-metrics — the daemon's own RSS / VSZ / CPU consumption
// (spec §3.3.3). All fields are best-effort; unavailable data
// is reported as 0 rather than a separate present flag.
struct SelfStats {
    uint64_t rss_bytes;          // current resident set; Linux only
    uint64_t peak_rss_bytes;     // peak ever, from getrusage(2)
    double   cpu_user_seconds;
    double   cpu_system_seconds;
};

// Thermal sensors — L3 / active tier (spec §3.3.3).
// Reports the hottest reading across every sensor we can probe.
//   * Linux:   /sys/class/thermal/thermal_zone*/temp (millidegrees C)
//   * FreeBSD: dev.cpu.<N>.temperature sysctl (Kelvin × 10)
// `present == false` when no sensors are exposed (containers, VMs
// without ACPI, jails). `max_celsius` and `sensor_count` stay 0 in
// that case.
struct ThermalStats {
    double   max_celsius;
    uint32_t sensor_count;
    bool     present;
};

struct Sample {
    uint64_t timestamp_nanos;
    Level    level;
    CpuStats  cpu;
    MemStats  mem;
    SwapStats swap;
    LoadStats load;
    DiskStats disk;
    NetStats  net;
    ProcessStats proc;
    EntropyStats entropy;
    SelfStats    self_;
    ThermalStats thermal;
    double    uptime_seconds;
};

} // namespace budyk
