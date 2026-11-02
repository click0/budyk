/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/memory.c — Linux memory metrics via /proc/meminfo. */

#include "core/sample_c.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ZFS on Linux: the ARC is memory the kernel gives back under pressure,
 * down to its minimum (c_min), and MemAvailable does not count it: a
 * root-on-ZFS host with a warm ARC read 40 points low and memory_low
 * fired, as on FreeBSD before its collector added the ARC. arcstats is
 * "name type value" lines; 0 when there is no ZFS. */
static unsigned long long zfs_arc_reclaimable(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/spl/kstat/zfs/arcstats", budyk_linux_proc_root());
    FILE* f = fopen(path, "re");
    if (f == NULL) return 0;
    unsigned long long size = 0, c_min = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        char name[64];
        unsigned type = 0;
        unsigned long long value = 0;
        if (sscanf(line, "%63s %u %llu", name, &type, &value) != 3) continue;
        if      (strcmp(name, "size")  == 0) size  = value;
        else if (strcmp(name, "c_min") == 0) c_min = value;
    }
    fclose(f);
    return size > c_min ? size - c_min : 0;
}

int budyk_collect_memory_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    char path[512];
    snprintf(path, sizeof(path), "%s/meminfo", budyk_linux_proc_root());
    FILE* f = fopen(path, "re");
    if (f == NULL) return -errno;

    unsigned long long total_kb = 0, available_kb = 0, swap_total_kb = 0, swap_free_kb = 0;
    unsigned long long free_kb = 0, buffers_kb = 0, cached_kb = 0, sreclaimable_kb = 0;
    int got_total = 0, got_avail = 0, got_swap_total = 0, got_swap_free = 0, got_free = 0;

    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        if      (!got_total      && strncmp(line, "MemTotal:",     9)  == 0) { sscanf(line + 9,  "%llu", &total_kb);      got_total = 1; }
        else if (!got_avail      && strncmp(line, "MemAvailable:", 13) == 0) { sscanf(line + 13, "%llu", &available_kb);  got_avail = 1; }
        else if (!got_free       && strncmp(line, "MemFree:",      8)  == 0) { sscanf(line + 8,  "%llu", &free_kb);       got_free = 1; }
        else if (strncmp(line, "Buffers:",      8)  == 0) sscanf(line + 8,  "%llu", &buffers_kb);
        else if (strncmp(line, "Cached:",       7)  == 0) sscanf(line + 7,  "%llu", &cached_kb);
        else if (strncmp(line, "SReclaimable:", 13) == 0) sscanf(line + 13, "%llu", &sreclaimable_kb);
        else if (!got_swap_total && strncmp(line, "SwapTotal:",   10)  == 0) { sscanf(line + 10, "%llu", &swap_total_kb); got_swap_total = 1; }
        else if (!got_swap_free  && strncmp(line, "SwapFree:",     9)  == 0) { sscanf(line + 9,  "%llu", &swap_free_kb);  got_swap_free = 1; }
    }
    fclose(f);

    if (!got_total) return -ENODATA;
    /* MemAvailable is from kernel 3.14: before it, free(1)'s own sum.
     * Both groups were left out for the daemon's life on such a host,
     * swap included, though every other key was there. */
    if (!got_avail) {
        if (!got_free) return -ENODATA;
        available_kb = free_kb + buffers_kb + cached_kb + sreclaimable_kb;
    }
    available_kb += zfs_arc_reclaimable() / 1024ULL;
    if (available_kb > total_kb) available_kb = total_kb;

    s->mem.total             = total_kb     * 1024ULL;
    s->mem.available         = available_kb * 1024ULL;
    s->mem.available_percent =
        total_kb > 0 ? (double)available_kb * 100.0 / (double)total_kb : 0.0;

    if (got_swap_total) {
        unsigned long long used_kb = swap_total_kb > swap_free_kb
                                   ? swap_total_kb - swap_free_kb : 0;
        s->swap.total        = swap_total_kb * 1024ULL;
        s->swap.used         = used_kb       * 1024ULL;
        s->swap.used_percent =
            swap_total_kb > 0 ? (double)used_kb * 100.0 / (double)swap_total_kb : 0.0;
    }
    return 0;
}
