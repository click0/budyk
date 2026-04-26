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
    s->mem.total             = (uint64_t)page_count * (uint64_t)page_size;
    s->mem.available         = avail_pages         * (uint64_t)page_size;
    s->mem.available_percent = page_count > 0
        ? (double)avail_pages * 100.0 / (double)page_count
        : 0.0;

    /* Swap — soft-fail to zero if kvm is unavailable. */
    s->swap.total        = 0;
    s->swap.used         = 0;
    s->swap.used_percent = 0.0;

    char errbuf[_POSIX2_LINE_MAX];
    errbuf[0] = '\0';
    kvm_t* kd = kvm_openfiles(NULL, "/dev/null", NULL, O_RDONLY, errbuf);
    if (kd != NULL) {
        struct kvm_swap kswap[16];
        int n = kvm_getswapinfo(kd, kswap, 16, 0);
        if (n > 0) {
            uint64_t total_pages = 0;
            uint64_t used_pages  = 0;
            for (int i = 0; i < n; ++i) {
                total_pages += (uint64_t)kswap[i].ksw_total;
                used_pages  += (uint64_t)kswap[i].ksw_used;
            }
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
