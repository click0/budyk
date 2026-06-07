/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/system.c — uptime (kern.boottime) and load (getloadavg). */

#include "core/sample_c.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Uptime: kern.boottime returns a `struct timeval` for the moment the
 * kernel started; subtracting it from the current monotonic-but-on-the-
 * wall-clock CLOCK_REALTIME yields seconds-since-boot. (CLOCK_UPTIME
 * exists on FreeBSD too but is per-process for jails — kern.boottime
 * is the host-wide value `top(1)` and `uptime(1)` use.)
 */
int budyk_collect_uptime_freebsd(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    struct timeval bt;
    size_t bt_len = sizeof(bt);
    if (sysctlbyname("kern.boottime", &bt, &bt_len, NULL, 0) != 0) return -errno;
    if (bt_len != sizeof(bt))                                     return -EIO;

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)                 return -errno;

    const double up = (double)(now.tv_sec - bt.tv_sec)
                    + (double)(now.tv_nsec - (long)bt.tv_usec * 1000) / 1.0e9;
    s->uptime_seconds = up < 0.0 ? 0.0 : up;
    return 0;
}

/* getloadavg(3) is in libc on FreeBSD as on Linux — it just queries
 * vm.loadavg + applies fscale internally. Use it directly so we don't
 * have to handle the fixed-point conversion ourselves.
 */
int budyk_collect_load_freebsd(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    double avg[3] = {0.0, 0.0, 0.0};
    int n = getloadavg(avg, 3);
    if (n != 3) return -EIO;

    s->load.avg_1m  = avg[0];
    s->load.avg_5m  = avg[1];
    s->load.avg_15m = avg[2];
    return 0;
}

/* Process counts via a single walk of kern.proc.all.
 *   total   — total length / sizeof(kinfo_proc)
 *   running — entries with ki_stat == SRUN
 *
 * We size the buffer with NULL+&bytes first (the FreeBSD idiom) then
 * malloc and read in one go. A race where new procs spawn between the
 * two calls means we might be off by ±1 — fine for monitoring; the
 * tick repeats in seconds.
 */
#include <sys/proc.h>     /* SRUN */
#include <sys/user.h>     /* struct kinfo_proc */

int budyk_collect_proc_freebsd(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    size_t bytes = 0;
    if (sysctlbyname("kern.proc.all", NULL, &bytes, NULL, 0) != 0) return -errno;
    if (bytes == 0) {
        s->proc.total   = 0;
        s->proc.running = 0;
        return 0;
    }

    struct kinfo_proc* procs = (struct kinfo_proc*)malloc(bytes);
    if (procs == NULL) return -ENOMEM;
    if (sysctlbyname("kern.proc.all", procs, &bytes, NULL, 0) != 0) {
        const int e = -errno;
        free(procs);
        return e;
    }

    const uint32_t n = (uint32_t)(bytes / sizeof(struct kinfo_proc));
    uint32_t running = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (procs[i].ki_stat == SRUN) ++running;
    }
    free(procs);

    s->proc.total   = n;
    s->proc.running = running;
    return 0;
}

/* FreeBSD has no /proc/sys/kernel/random/entropy_avail equivalent.
 * `kern.random.harvest.*` exposes harvest sources (boolean flags),
 * not pool depth in bits — different semantics from the Linux number.
 * Spec §3.3.3 explicitly says "either omit or use kern.random.*" —
 * we omit by reporting present=0; the SPA / Lua bindings handle this
 * gracefully.
 */
int budyk_collect_entropy_freebsd(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;
    s->entropy.available_bits = 0;
    s->entropy.present        = 0;
    return 0;
}

/* Self-metrics on FreeBSD:
 *   peak_rss_bytes / cpu_*_seconds — getrusage(RUSAGE_SELF). ru_maxrss
 *     is in KiB on FreeBSD ≥ 9.
 *   rss_bytes — sysctl KERN_PROC_PID on our own pid → kinfo_proc.ki_rssize
 *     (pages) × getpagesize().
 *
 * Note: FreeBSD updates ru_maxrss lazily — the value can legitimately
 * be 0 for a process that just started and hasn't touched many pages.
 * Tests must not assert peak_rss_bytes > 0 unconditionally. ki_rssize
 * is sampled live and is reliable.
 */
#include <sys/resource.h>
#include <unistd.h>      /* getpid, getpagesize */

int budyk_collect_self_freebsd(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    s->self_.rss_bytes          = 0;
    s->self_.peak_rss_bytes     = 0;
    s->self_.cpu_user_seconds   = 0.0;
    s->self_.cpu_system_seconds = 0.0;

    /* Current RSS via kinfo_proc for our own PID. ki_rssize is in
     * VM_PAGE_SIZE units. Failure here is non-fatal — we just leave
     * rss_bytes at 0 and continue with getrusage data. */
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };
    struct kinfo_proc kp;
    size_t kp_len = sizeof(kp);
    if (sysctl(mib, 4, &kp, &kp_len, NULL, 0) == 0 && kp_len == sizeof(kp)) {
        s->self_.rss_bytes = (uint64_t)kp.ki_rssize *
                             (uint64_t)getpagesize();
    }

    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        s->self_.peak_rss_bytes     = (uint64_t)ru.ru_maxrss * 1024ULL;
        s->self_.cpu_user_seconds   =
            (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1.0e6;
        s->self_.cpu_system_seconds =
            (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1.0e6;
    }
    return 0;
}

/* Thermal — walk the dev.cpu.<N>.temperature sysctl chain.
 * Each value is reported as Kelvin × 10 (e.g. 3151 = 42.0°C).
 * Stop on the first sysctl that fails; that's how many cores have
 * thermal monitoring exposed on this host.
 *
 * Many guests (jails, virt without ACPI/IPMI passthrough) won't
 * expose any temperature sysctls — the collector reports
 * present=0 in that case.
 */
#include <stdio.h>
#include <string.h>

int budyk_collect_thermal_freebsd(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    s->thermal.max_celsius  = 0.0;
    s->thermal.sensor_count = 0;
    s->thermal.present      = 0;

    double  hottest = -1e9;
    uint32_t count  = 0;

    for (int n = 0; n < 256; ++n) {
        char mib[48];
        int  m = snprintf(mib, sizeof(mib), "dev.cpu.%d.temperature", n);
        if (m <= 0 || (size_t)m >= sizeof(mib)) break;

        int    raw = 0;
        size_t sz  = sizeof(raw);
        if (sysctlbyname(mib, &raw, &sz, NULL, 0) != 0) break;

        const double celsius = (double)raw / 10.0 - 273.15;
        if (celsius > hottest) hottest = celsius;
        ++count;
    }

    if (count > 0) {
        s->thermal.max_celsius  = hottest;
        s->thermal.sensor_count = count;
        s->thermal.present      = 1;
    }
    return 0;
}
