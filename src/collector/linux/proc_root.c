/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/proc_root.c — where the CPU, disk, network,
 * netstat, process and filesystem collectors find /proc. Tests point it at a fixture tree. */

#include "core/sample_c.h"

#include <stdio.h>

static char g_root[256] = "/proc";

void budyk_linux_set_proc_root(const char* root) {
    snprintf(g_root, sizeof(g_root), "%s", root != NULL ? root : "/proc");
}

const char* budyk_linux_proc_root(void) {
    return g_root;
}

static char g_sys_root[256] = "/sys";

void budyk_linux_set_sys_root(const char* root) {
    snprintf(g_sys_root, sizeof(g_sys_root), "%s", root != NULL ? root : "/sys");
}

const char* budyk_linux_sys_root(void) {
    return g_sys_root;
}
