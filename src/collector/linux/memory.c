/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/memory.c — Linux memory metrics via /proc, /sys */

#include <stdio.h>
#include <errno.h>

int budyk_collect_memory_linux(void* sample) {
    (void)sample;
    return -1; /* ENOSYS — stub */
}
