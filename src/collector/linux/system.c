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
