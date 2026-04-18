// SPDX-License-Identifier: BSD-3-Clause
#ifndef BUDYK_SAMPLE_C_H
#define BUDYK_SAMPLE_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// C-compatible view of budyk::Sample. Lets plain-C collectors
// (src/collector/<platform>/*.c) populate samples without pulling in
// C++ headers. The C++ side is responsible for converting between
// budyk_sample_c and budyk::Sample when wiring the MetricSource.
typedef struct {
    double   total_percent;
    uint32_t count;
} budyk_cpu_c;

typedef struct {
    uint64_t total;
    uint64_t available;
    double   available_percent;
} budyk_mem_c;

typedef struct {
    uint64_t total;
    uint64_t used;
    double   used_percent;
} budyk_swap_c;

typedef struct {
    double avg_1m;
    double avg_5m;
    double avg_15m;
} budyk_load_c;

typedef struct {
    uint64_t read_bytes_per_sec;
    uint64_t write_bytes_per_sec;
    uint32_t device_count;
} budyk_disk_c;

typedef struct {
    uint64_t rx_bytes_per_sec;
    uint64_t tx_bytes_per_sec;
    uint32_t interface_count;
} budyk_net_c;

typedef struct {
    uint64_t     timestamp_nanos;
    uint8_t      level;
    budyk_cpu_c  cpu;
    budyk_mem_c  mem;
    budyk_swap_c swap;
    budyk_load_c load;
    budyk_disk_c disk;
    budyk_net_c  net;
    double       uptime_seconds;
} budyk_sample_c;

// Linux collectors. Return 0 on success, negative errno on failure.
int budyk_collect_memory_linux(budyk_sample_c* s);
int budyk_collect_load_linux  (budyk_sample_c* s);
int budyk_collect_uptime_linux(budyk_sample_c* s);

// CPU collection requires state across ticks — the ctx carries the
// previous /proc/stat snapshot for delta computation. Initialise the
// struct to all zeros before the first call. The first call records
// a baseline and sets cpu.total_percent to 0.
typedef struct {
    uint64_t busy;      // user + nice + system + irq + softirq + steal
    uint64_t total;     // busy + idle + iowait
    int      has_prev;
} budyk_cpu_ctx_c;

int budyk_collect_cpu_linux(budyk_cpu_ctx_c* ctx, budyk_sample_c* s);

// Disk / network collection also needs across-tick state — both accumulate
// monotonic byte counters and compute per-second rates from the delta.
// Caller must set s->timestamp_nanos before the call; collector uses the
// elapsed wall-clock since the previous call to divide the byte delta.
// First call records the baseline and sets rates to 0.
typedef struct {
    uint64_t prev_read_sectors;   // summed across whole-disk devices
    uint64_t prev_write_sectors;
    uint64_t prev_ns;
    int      has_prev;
} budyk_disk_ctx_c;

typedef struct {
    uint64_t prev_rx_bytes;       // summed across non-loopback interfaces
    uint64_t prev_tx_bytes;
    uint64_t prev_ns;
    int      has_prev;
} budyk_net_ctx_c;

int budyk_collect_disk_linux   (budyk_disk_ctx_c* ctx, budyk_sample_c* s);
int budyk_collect_network_linux(budyk_net_ctx_c*  ctx, budyk_sample_c* s);

#ifdef __cplusplus
}
#endif
#endif // BUDYK_SAMPLE_C_H
