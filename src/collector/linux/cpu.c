/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/cpu.c — per-tick CPU% via /proc/stat deltas. */

#include "core/sample_c.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* /proc/stat aggregate line:
 *   cpu  user nice system idle iowait irq softirq steal guest guest_nice
 * Per-core lines follow as "cpu0", "cpu1", ...  Counts are in USER_HZ.
 */
int budyk_collect_cpu_linux(budyk_cpu_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;

    FILE* f = fopen("/proc/stat", "r");
    if (f == NULL) return -errno;

    unsigned long long user = 0, nice = 0, system_ = 0, idle = 0,
                       iowait = 0, irq = 0, softirq = 0, steal = 0;

    char line[512];
    int  got_agg = 0;
    uint32_t ncpu = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        if (!got_agg && strncmp(line, "cpu ", 4) == 0) {
            /* guest + guest_nice intentionally ignored — they're already
             * counted in user / nice on modern kernels.
             */
            sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &system_, &idle,
                   &iowait, &irq, &softirq, &steal);
            got_agg = 1;
        } else if (strncmp(line, "cpu", 3) == 0 &&
                   line[3] >= '0' && line[3] <= '9') {
            ++ncpu;
        } else if (strncmp(line, "intr", 4) == 0) {
            break;  /* no more cpuN lines after this */
        }
    }
    fclose(f);
    if (!got_agg) return -ENODATA;

    const uint64_t busy  = user + nice + system_ + irq + softirq + steal;
    const uint64_t total = busy + idle + iowait;

    s->cpu.count = ncpu;

    if (!ctx->has_prev) {
        s->cpu.total_percent = 0.0;
    } else {
        uint64_t db = busy  >= ctx->busy  ? busy  - ctx->busy  : 0;
        uint64_t dt = total >= ctx->total ? total - ctx->total : 0;
        s->cpu.total_percent = dt > 0
            ? (double)db * 100.0 / (double)dt
            : 0.0;
    }

    ctx->busy     = busy;
    ctx->total    = total;
    ctx->has_prev = 1;
    return 0;
}
