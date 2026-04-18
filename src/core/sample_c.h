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
    uint64_t     timestamp_nanos;
    uint8_t      level;
    budyk_cpu_c  cpu;
    budyk_mem_c  mem;
    budyk_swap_c swap;
    budyk_load_c load;
    double       uptime_seconds;
} budyk_sample_c;

// Linux collectors. Return 0 on success, negative errno on failure.
int budyk_collect_memory_linux(budyk_sample_c* s);
int budyk_collect_load_linux  (budyk_sample_c* s);
int budyk_collect_uptime_linux(budyk_sample_c* s);

#ifdef __cplusplus
}
#endif
#endif // BUDYK_SAMPLE_C_H
