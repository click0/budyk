/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/network.c — Linux network metrics via /proc, /sys */

#include <stdio.h>
#include <errno.h>

int budyk_collect_network_linux(void* sample) {
    (void)sample;
    return -1; /* ENOSYS — stub */
}
