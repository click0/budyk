/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/system.c — uptime (/proc/uptime) and load (getloadavg). */

#include "core/sample_c.h"

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
