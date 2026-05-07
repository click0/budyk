/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/system.c — uptime (/proc/uptime) and load (getloadavg). */

#include "core/sample_c.h"

#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int budyk_collect_uptime_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    FILE* f = fopen("/proc/uptime", "r");
    if (f == NULL) return -errno;

    double up = 0.0;
    int n = fscanf(f, "%lf", &up);
    fclose(f);
    if (n != 1) return -EIO;

    s->uptime_seconds = up;
    return 0;
}

int budyk_collect_load_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    double avg[3] = {0.0, 0.0, 0.0};
    int n = getloadavg(avg, 3);
    if (n != 3) return -EIO;

    s->load.avg_1m  = avg[0];
    s->load.avg_5m  = avg[1];
    s->load.avg_15m = avg[2];
    return 0;
}

/* /proc/sys/kernel/random/entropy_avail — single integer, the number
 * of bits currently in the kernel's CSPRNG entropy pool. Populated on
 * every Linux >= 2.6. On stripped containers the file may be missing
 * (read-only / unsupported sysctl) — soft-fail with present=0.
 */
int budyk_collect_entropy_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    FILE* f = fopen("/proc/sys/kernel/random/entropy_avail", "r");
    if (f == NULL) {
        s->entropy.available_bits = 0;
        s->entropy.present        = 0;
        return 0;                  /* missing on this host — non-fatal */
    }
    unsigned int bits = 0;
    int n = fscanf(f, "%u", &bits);
    fclose(f);
    if (n != 1) {
        s->entropy.available_bits = 0;
        s->entropy.present        = 0;
        return 0;
    }
    s->entropy.available_bits = bits;
    s->entropy.present        = 1;
    return 0;
}

/* /proc/loadavg layout (kernel ≥ 2.6):
 *   <load1> <load5> <load15> <running>/<total> <last_pid>
 *
 * Field 4 ("running/total") gives us both numbers cheaply, no /proc
 * directory walk required.
 */
int budyk_collect_proc_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    FILE* f = fopen("/proc/loadavg", "r");
    if (f == NULL) return -errno;

    double a, b, c;
    unsigned int running = 0, total = 0;
    int n = fscanf(f, "%lf %lf %lf %u/%u", &a, &b, &c, &running, &total);
    fclose(f);
    if (n != 5) return -EIO;

    s->proc.running = running;
    s->proc.total   = total;
    return 0;
}

/* Self-metrics — the daemon's own resource consumption.
 * Linux:
 *   * /proc/self/statm field 1 ("size" in pages) → vsz, but we surface
 *     RSS instead via field 2 ("resident" in pages); multiply by
 *     sysconf(_SC_PAGESIZE).
 *   * getrusage(RUSAGE_SELF) for ru_maxrss (peak, KiB) and the
 *     user/system CPU time accumulators.
 * The fields are best-effort — soft-fail to 0 on any failure.
 */
int budyk_collect_self_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    s->self_.rss_bytes          = 0;
    s->self_.peak_rss_bytes     = 0;
    s->self_.cpu_user_seconds   = 0.0;
    s->self_.cpu_system_seconds = 0.0;

    /* Current RSS. */
    FILE* f = fopen("/proc/self/statm", "r");
    if (f != NULL) {
        unsigned long long sz_pages = 0, rss_pages = 0;
        if (fscanf(f, "%llu %llu", &sz_pages, &rss_pages) == 2) {
            const long page = sysconf(_SC_PAGESIZE);
            if (page > 0) {
                s->self_.rss_bytes = (uint64_t)rss_pages * (uint64_t)page;
            }
        }
        fclose(f);
    }

    /* Peak RSS + CPU time. */
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        /* ru_maxrss: KiB on Linux. */
        s->self_.peak_rss_bytes     = (uint64_t)ru.ru_maxrss * 1024ULL;
        s->self_.cpu_user_seconds   =
            (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1.0e6;
        s->self_.cpu_system_seconds =
            (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1.0e6;
    }
    return 0;
}
