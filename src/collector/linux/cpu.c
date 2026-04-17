/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/cpu.c — Linux cpu metrics via /proc, /sys */

#include <stdio.h>
#include <errno.h>

int budyk_collect_cpu_linux(void* sample) {
    (void)sample;
    return -1; /* ENOSYS — stub */
}
