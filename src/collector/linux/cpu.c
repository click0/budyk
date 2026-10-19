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
/* One core's share of each part since its previous read, and its busy
 * total; parts in budyk_cpu_ctx_c.part order, total in [6]. */
static void core_percent(const uint64_t now[7], const uint64_t prev[7], int has_prev,
                         budyk_core_c* out) {
    memset(out, 0, sizeof(*out));
    if (!has_prev) return;
    const uint64_t dt = now[6] >= prev[6] ? now[6] - prev[6] : 0;
    if (dt == 0) return;
    double pct[6];
    for (int i = 0; i < 6; ++i) {
        const uint64_t d = now[i] >= prev[i] ? now[i] - prev[i] : 0;
        pct[i] = (double)d * 100.0 / (double)dt;
    }
    out->user_percent   = pct[0];
    out->system_percent = pct[1];
    out->iowait_percent = pct[2];
    out->irq_percent    = pct[3];
    out->steal_percent  = pct[4];
    out->idle_percent   = pct[5];
    out->total_percent  = 100.0 - pct[5] - pct[2];
    if (out->total_percent < 0.0) out->total_percent = 0.0;
}

int budyk_collect_cpu_linux(budyk_cpu_ctx_c* ctx, budyk_sample_c* s) {
    return budyk_collect_cpu_linux_ex(ctx, s, NULL);
}

int budyk_collect_cpu_linux_ex(budyk_cpu_ctx_c* ctx, budyk_sample_c* s, budyk_detail_c* d) {
    if (ctx == NULL || s == NULL) return -EINVAL;
    if (d != NULL) d->ncores = 0;

    char path[512];
    snprintf(path, sizeof(path), "%s/stat", budyk_linux_proc_root());
    FILE* f = fopen(path, "re");
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
            /* cpuN, in order; an offline core has no line. Its prev
             * slot is its number, so a core coming back gets a baseline
             * only if it was gone for a whole tick. */
            unsigned id = 0;
            unsigned long long u = 0, n = 0, sy = 0, id_ = 0, io = 0, iq = 0, sq = 0, st = 0;
            if (d != NULL &&
                sscanf(line + 3, "%u %llu %llu %llu %llu %llu %llu %llu %llu",
                       &id, &u, &n, &sy, &id_, &io, &iq, &sq, &st) == 9 &&
                id < BUDYK_MAX_CORES && d->ncores < BUDYK_MAX_CORES) {
                const uint64_t now[7] = { u + n, sy, io, iq + sq, st, id_,
                                          u + n + sy + io + iq + sq + st + id_ };
                core_percent(now, d->core_prev[id], d->core_has_prev[id],
                             &d->cores[d->ncores++]);
                memcpy(d->core_prev[id], now, sizeof(now));
                d->core_has_prev[id] = 1;
            }
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
            const uint64_t dp = part[i] >= ctx->part[i] ? part[i] - ctx->part[i] : 0;
            pct[i] = dt > 0 ? (double)dp * 100.0 / (double)dt : 0.0;
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
