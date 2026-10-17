/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/cpu.c — per-tick CPU% via kern.cp_time deltas. */

#include "core/sample_c.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/resource.h>   /* CPUSTATES, CP_USER, CP_NICE, CP_SYS, CP_INTR, CP_IDLE */

#include <errno.h>
#include <string.h>

/* kern.cp_time returns long[CPUSTATES] (== 5 on every supported FreeBSD):
 *   CP_USER  user-mode ticks
 *   CP_NICE  user-mode ticks of nice'd processes
 *   CP_SYS   kernel-mode ticks
 *   CP_INTR  hard-interrupt ticks
 *   CP_IDLE  idle ticks
 *
 * Counts are in stathz ticks; the absolute scale doesn't matter — only
 * the busy/total ratio between consecutive snapshots does.
 *
 * hw.ncpu reports the number of online CPUs. Honours `cpuset(1) -l`
 * masks (with the same kernel-side caveats vmstat / top accept).
 */
int budyk_collect_cpu_freebsd(budyk_cpu_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;

    long cp_time[CPUSTATES];
    size_t cp_len = sizeof(cp_time);
    if (sysctlbyname("kern.cp_time", cp_time, &cp_len, NULL, 0) != 0) {
        return -errno;
    }
    if (cp_len != sizeof(cp_time)) return -EIO;

    int    ncpu     = 0;
    size_t ncpu_len = sizeof(ncpu);
    if (sysctlbyname("hw.ncpu", &ncpu, &ncpu_len, NULL, 0) != 0) {
        return -errno;
    }
    if (ncpu < 1) ncpu = 1;
    s->cpu.count = (uint32_t)ncpu;

    const uint64_t busy  = (uint64_t)cp_time[CP_USER]
                         + (uint64_t)cp_time[CP_NICE]
                         + (uint64_t)cp_time[CP_SYS]
                         + (uint64_t)cp_time[CP_INTR];
    const uint64_t total = busy + (uint64_t)cp_time[CP_IDLE];
    /* The parts, in budyk_cpu_ctx_c.part order. FreeBSD has no iowait
     * or steal state: those stay 0. */
    const uint64_t part[6] = {
        (uint64_t)cp_time[CP_USER] + (uint64_t)cp_time[CP_NICE],
        (uint64_t)cp_time[CP_SYS], 0, (uint64_t)cp_time[CP_INTR], 0,
        (uint64_t)cp_time[CP_IDLE],
    };

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
