/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/network.c — FreeBSD network metrics via sysctl/devstat/kvm */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <errno.h>

int budyk_collect_network_freebsd(void* sample) {
    (void)sample;
    return -1; /* ENOSYS — stub */
}
