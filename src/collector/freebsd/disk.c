/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/disk.c — aggregate per-second disk I/O via devstat(3).
 *
 * devstat_getdevs(3) returns an array of struct devstat covering every
 * GEOM provider the kernel exports. We filter to whole disks
 * (DEVSTAT_TYPE_DIRECT, no DEVSTAT_TYPE_PASS bit) and sum bytes-read /
 * bytes-written across the set. Per-second rates are computed from the
 * delta to the previous tick, using the wall-clock elapsed in
 * s->timestamp_nanos (set by the caller before this call).
 *
 * libdevstat is part of FreeBSD base; budyk_collector links -ldevstat
 * via CMakeLists.txt on BUDYK_PLATFORM=freebsd.
 */

#include "core/sample_c.h"

#include <sys/types.h>
#include <devstat.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int budyk_collect_disk_freebsd(budyk_disk_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;

    /* libdevstat keeps an internal version; bail if it disagrees with
     * the kernel running underneath us (running a binary built against
     * one major against another's kernel is the usual offender). */
    if (devstat_checkversion(NULL) < 0) return -EIO;

    struct statinfo stats;
    memset(&stats, 0, sizeof(stats));
    stats.dinfo = (struct devinfo*)calloc(1, sizeof(struct devinfo));
    if (stats.dinfo == NULL) return -ENOMEM;

    if (devstat_getdevs(NULL, &stats) < 0) {
        free(stats.dinfo);
        return -EIO;
    }

    uint64_t total_read  = 0;
    uint64_t total_write = 0;
    uint32_t devs        = 0;
    for (int i = 0; i < stats.dinfo->numdevs; ++i) {
        const struct devstat* d = &stats.dinfo->devices[i];
        if ((d->device_type & DEVSTAT_TYPE_MASK) != DEVSTAT_TYPE_DIRECT) continue;
        if (d->device_type & DEVSTAT_TYPE_PASS) continue;
        if (d->device_name[0] == '\0') continue;
        total_read  += d->bytes[DEVSTAT_READ];
        total_write += d->bytes[DEVSTAT_WRITE];
        ++devs;
    }

    /* dinfo->mem_ptr is owned by devstat — free it before our calloc. */
    if (stats.dinfo->mem_ptr != NULL) free(stats.dinfo->mem_ptr);
    free(stats.dinfo);

    /* The Linux side uses the same ctx struct fields for raw sector
     * deltas; on FreeBSD we just stash bytes there — fields are opaque
     * state, not part of the wire format. */
    s->disk.device_count       = devs;
    s->disk.read_bytes_per_sec = 0;
    s->disk.write_bytes_per_sec = 0;

    if (ctx->has_prev) {
        const uint64_t now_ns = s->timestamp_nanos;
        const uint64_t dt_ns  = (now_ns > ctx->prev_ns) ? (now_ns - ctx->prev_ns) : 0;
        if (dt_ns > 0) {
            /* Counters can roll on hot-unplug → reattach. Skip the
             * delta on a backwards step rather than emit a giant rate. */
            if (total_read  >= ctx->prev_read_sectors) {
                s->disk.read_bytes_per_sec =
                    (total_read - ctx->prev_read_sectors) * 1000000000ULL / dt_ns;
            }
            if (total_write >= ctx->prev_write_sectors) {
                s->disk.write_bytes_per_sec =
                    (total_write - ctx->prev_write_sectors) * 1000000000ULL / dt_ns;
            }
        }
    }

    ctx->prev_read_sectors  = total_read;
    ctx->prev_write_sectors = total_write;
    ctx->prev_ns            = s->timestamp_nanos;
    ctx->has_prev           = 1;
    return 0;
}
