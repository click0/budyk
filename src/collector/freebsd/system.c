/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/system.c — uptime (kern.boottime) and load (getloadavg). */

#include "core/sample_c.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* Processes by state via a single walk of kern.proc.proc (one
 * kinfo_proc per process; kern.proc.all has one per thread, as ps -H
 * shows them): SRUN running; SSLEEP, SWAIT (waiting for an
 * interrupt) and SIDL (being created) sleeping; SLOCK blocked on a lock;
 * SSTOP stopped; SZOMB zombie. ki_numthreads sums to the thread count.
 *
 * We size the buffer with NULL+&bytes first (the FreeBSD idiom), with
 * room for processes forked before the second call, then read in one
 * go; ENOMEM (more than that appeared) retries once.
 */
#include <sys/proc.h>     /* SRUN, SSLEEP, ... */
#include <sys/user.h>     /* struct kinfo_proc */
#include <sys/timex.h>    /* ntp_adjtime */

int budyk_collect_proc_freebsd(budyk_sample_c* s) {
    return budyk_collect_proc_freebsd_ex(s, NULL);
}

/* A process's state as ps(1) shows it. */
static char state_letter(int stat) {
    switch (stat) {
        case SRUN:   return 'R';
        case SSLEEP: return 'S';
        case SWAIT:  return 'W';
        case SLOCK:  return 'L';
        case SSTOP:  return 'T';
        case SZOMB:  return 'Z';
        case SIDL:   return 'I';
        default:     return '?';
    }
}

/* With d, also the top processes: CPU time from ki_runtime (µs), told
 * apart from a reused pid by ki_start; system processes (P_SYSTEM: the
 * kernel, idle, interrupt threads) left out, as top(1) does without -S. */
int budyk_collect_proc_freebsd_ex(budyk_sample_c* s, budyk_detail_c* d) {
    if (s == NULL) return -EINVAL;
    memset(&s->proc, 0, sizeof(s->proc));

    struct kinfo_proc* procs = NULL;
    size_t bytes = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        bytes = 0;
        if (sysctlbyname("kern.proc.proc", NULL, &bytes, NULL, 0) != 0) return -errno;
        if (bytes == 0) return 0;
        bytes += bytes / 8;
        procs = (struct kinfo_proc*)malloc(bytes);
        if (procs == NULL) return -ENOMEM;
        if (sysctlbyname("kern.proc.proc", procs, &bytes, NULL, 0) == 0) break;
        const int e = errno;
        free(procs);
        procs = NULL;
        if (e != ENOMEM) return -e;
    }
    if (procs == NULL) return -ENOMEM;

    const size_t n = bytes / sizeof(struct kinfo_proc);
    const uint64_t page = (uint64_t)getpagesize();
    if (d != NULL) budyk_procs_begin(d, s->timestamp_nanos);
    for (size_t i = 0; i < n; ++i) {
        const struct kinfo_proc* kp = &procs[i];
        switch (kp->ki_stat) {
            case SRUN:                         ++s->proc.running;  break;
            case SSLEEP: case SWAIT: case SIDL: ++s->proc.sleeping; break;
            case SLOCK:                        ++s->proc.blocked;  break;
            case SSTOP:                        ++s->proc.stopped;  break;
            case SZOMB:                        ++s->proc.zombie;   break;
            default:                           break;
        }
        s->proc.threads += (uint32_t)kp->ki_numthreads;
        if (d == NULL || (kp->ki_flag & P_SYSTEM) != 0) continue;
        budyk_proc_read_c r;
        memset(&r, 0, sizeof(r));
        r.pid       = (int32_t)kp->ki_pid;
        r.start     = (uint64_t)kp->ki_start.tv_sec * 1000000u + (uint64_t)kp->ki_start.tv_usec;
        r.cpu       = (uint64_t)kp->ki_runtime;
        r.rss_bytes = (uint64_t)kp->ki_rssize * page;
        r.threads   = (uint32_t)kp->ki_numthreads;
        r.state     = state_letter(kp->ki_stat);
        snprintf(r.name, sizeof(r.name), "%s", kp->ki_comm);
        budyk_procs_add(d, &r, 1.0e6);
    }
    if (d != NULL) budyk_procs_end(d);
    free(procs);
    s->proc.total = (uint32_t)n;
    return 0;
}

/* Clock sync: the kernel's NTP discipline from ntp_adjtime(2) with no
 * mode bits (read only, no privilege needed). Synchronised unless the
 * kernel says TIME_ERROR or STA_UNSYNC; offset in microseconds, or
 * nanoseconds under STA_NANO; maxerror in microseconds.
 */
int budyk_collect_clock_freebsd(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;
    memset(&s->clock, 0, sizeof(s->clock));
    struct timex tx;
    memset(&tx, 0, sizeof(tx));
    const int state = ntp_adjtime(&tx);
    if (state < 0) return 0;          /* soft-fail: present stays 0 */
    s->clock.present      = 1;
    s->clock.synced       = (uint8_t)(state != TIME_ERROR && (tx.status & STA_UNSYNC) == 0);
    s->clock.offset_ms    = (tx.status & STA_NANO) != 0 ? (double)tx.offset / 1.0e6
                                                        : (double)tx.offset / 1.0e3;
    s->clock.max_error_ms = (double)tx.maxerror / 1.0e3;
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
