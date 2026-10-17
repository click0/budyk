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

    char path[512];
    snprintf(path, sizeof(path), "%s/stat", budyk_linux_proc_root());
    FILE* f = fopen(path, "r");
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
    /* The parts, in budyk_cpu_ctx_c.part order. */
    const uint64_t part[6] = { user + nice, system_, iowait, irq + softirq, steal, idle };

    s->cpu.count = ncpu;

    double pct[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (!ctx->has_prev) {
        s->cpu.total_percent = 0.0;
    } else {
        uint64_t db = busy  >= ctx->busy  ? busy  - ctx->busy  : 0;
        uint64_t dt = total >= ctx->total ? total - ctx->total : 0;
        s->cpu.total_percent = dt > 0
            ? (double)db * 100.0 / (double)dt
            : 0.0;
        for (int i = 0; i < 6; ++i) {
            const uint64_t d = part[i] >= ctx->part[i] ? part[i] - ctx->part[i] : 0;
            pct[i] = dt > 0 ? (double)d * 100.0 / (double)dt : 0.0;
        }
    }
    s->cpu.user_percent   = pct[0];
    s->cpu.system_percent = pct[1];
    s->cpu.iowait_percent = pct[2];
    s->cpu.irq_percent    = pct[3];
    s->cpu.steal_percent  = pct[4];
    s->cpu.idle_percent   = pct[5];

    for (int i = 0; i < 6; ++i) ctx->part[i] = part[i];
    ctx->busy     = busy;
    ctx->total    = total;
    ctx->has_prev = 1;
    return 0;
}
