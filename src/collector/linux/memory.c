/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/memory.c — Linux memory metrics via /proc/meminfo. */

#include "core/sample_c.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int budyk_collect_memory_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    FILE* f = fopen("/proc/meminfo", "r");
    if (f == NULL) return -errno;

    unsigned long long total_kb = 0, available_kb = 0, swap_total_kb = 0, swap_free_kb = 0;
    int got_total = 0, got_avail = 0, got_swap_total = 0, got_swap_free = 0;

    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        if      (!got_total      && strncmp(line, "MemTotal:",     9)  == 0) { sscanf(line + 9,  "%llu", &total_kb);      got_total = 1; }
        else if (!got_avail      && strncmp(line, "MemAvailable:", 13) == 0) { sscanf(line + 13, "%llu", &available_kb);  got_avail = 1; }
        else if (!got_swap_total && strncmp(line, "SwapTotal:",   10)  == 0) { sscanf(line + 10, "%llu", &swap_total_kb); got_swap_total = 1; }
        else if (!got_swap_free  && strncmp(line, "SwapFree:",     9)  == 0) { sscanf(line + 9,  "%llu", &swap_free_kb);  got_swap_free = 1; }
        if (got_total && got_avail && got_swap_total && got_swap_free) break;
    }
    fclose(f);

    if (!got_total || !got_avail) return -ENODATA;

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
