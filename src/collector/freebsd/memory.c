/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/memory.c — RAM via vm.stats.vm.* + swap via kvm. */

#include "core/sample_c.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <kvm.h>
#include <limits.h>
#include <unistd.h>

#include <errno.h>
#include <string.h>

/* RAM accounting on FreeBSD differs from Linux's MemAvailable: the kernel
 * exposes per-state page counts and the caller sums up the reclaimable
 * ones — free + inactive + cache + laundry — to approximate "would-be-
 * free if pressure rose". This matches the math `top(1)` and `vmstat(8)`
 * print as the "Free + Inact + Cache + Laundry" headline.
 *
 * Swap is read via libkvm. Opening kvm with /dev/null as the kernel
 * image is the documented "no special privilege required" path; if that
 * still fails (jails, stripped containers) the swap fields stay zero
 * and the rest of the memory sample is reported.
 */
static int read_u32(const char* mib, uint32_t* out) {
    size_t sz = sizeof(*out);
    return sysctlbyname(mib, out, &sz, NULL, 0);
}

int budyk_collect_memory_freebsd(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    uint32_t page_size = 0;
    if (read_u32("vm.stats.vm.v_page_size", &page_size) != 0) return -errno;
    if (page_size == 0)                                       return -EIO;

    uint32_t page_count = 0;
    if (read_u32("vm.stats.vm.v_page_count", &page_count) != 0) return -errno;

    uint32_t free_count = 0;
    if (read_u32("vm.stats.vm.v_free_count", &free_count) != 0) return -errno;

    /* Optional fields — older / configured-out kernels may not export them. */
    uint32_t inact = 0, cache = 0, laundry = 0;
    (void)read_u32("vm.stats.vm.v_inactive_count", &inact);
    (void)read_u32("vm.stats.vm.v_cache_count",    &cache);
    (void)read_u32("vm.stats.vm.v_laundry_count",  &laundry);

    const uint64_t avail_pages = (uint64_t)free_count
                               + (uint64_t)inact
                               + (uint64_t)cache
                               + (uint64_t)laundry;
    const uint64_t total = (uint64_t)page_count * (uint64_t)page_size;
    uint64_t avail = avail_pages * (uint64_t)page_size;

    /* The ZFS ARC is wired memory the kernel gives back under pressure,
     * down to its minimum (c_min), and is counted in: a warm ARC holds
     * most of the RAM of a ZFS host, available read under 10 % on a
     * healthy machine, and the shipped memory_low rule fired. No ZFS, no
     * such sysctl, nothing added. */
    uint64_t arc_size = 0, arc_min = 0;
    size_t sz = sizeof(arc_size);
    if (sysctlbyname("kstat.zfs.misc.arcstats.size", &arc_size, &sz, NULL, 0) == 0 &&
        sz == sizeof(arc_size)) {
        sz = sizeof(arc_min);
        if (sysctlbyname("kstat.zfs.misc.arcstats.c_min", &arc_min, &sz, NULL, 0) != 0 ||
            sz != sizeof(arc_min)) {
            arc_min = 0;
        }
        if (arc_size > arc_min) avail += arc_size - arc_min;
    }
    if (avail > total) avail = total;

    s->mem.total             = total;
    s->mem.available         = avail;
    s->mem.available_percent = total > 0
        ? (double)avail * 100.0 / (double)total
        : 0.0;

    /* Swap — soft-fail to zero if kvm is unavailable. */
    s->swap.total        = 0;
    s->swap.used         = 0;
    s->swap.used_percent = 0.0;

    char errbuf[_POSIX2_LINE_MAX];
    errbuf[0] = '\0';
    kvm_t* kd = kvm_openfiles(NULL, "/dev/null", NULL, O_RDONLY, errbuf);
    if (kd != NULL) {
        /* An array of one: no device entries, only the entry kvm adds after
         * them with the total of every device. Summing per-device entries
         * missed those past the array (16 held 15). */
        struct kvm_swap all;
        memset(&all, 0, sizeof(all));
        if (kvm_getswapinfo(kd, &all, 1, 0) >= 0) {
            const uint64_t total_pages = (uint64_t)all.ksw_total;
            const uint64_t used_pages  = (uint64_t)all.ksw_used;
            s->swap.total = total_pages * (uint64_t)page_size;
            s->swap.used  = used_pages  * (uint64_t)page_size;
            s->swap.used_percent = total_pages > 0
                ? (double)used_pages * 100.0 / (double)total_pages
                : 0.0;
        }
        kvm_close(kd);
    }
    return 0;
}
