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
