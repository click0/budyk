/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/system.c — Linux system metrics via /proc, /sys */

#include <stdio.h>
#include <errno.h>

int budyk_collect_system_linux(void* sample) {
    (void)sample;
    return -1; /* ENOSYS — stub */
}
