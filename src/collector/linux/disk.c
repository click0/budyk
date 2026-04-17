/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/disk.c — Linux disk metrics via /proc, /sys */

#include <stdio.h>
#include <errno.h>

int budyk_collect_disk_linux(void* sample) {
    (void)sample;
    return -1; /* ENOSYS — stub */
}
