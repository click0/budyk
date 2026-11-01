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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int budyk_collect_disk_freebsd(budyk_disk_ctx_c* ctx, budyk_sample_c* s) {
    return budyk_collect_disk_freebsd_ex(ctx, s, NULL);
}

/* md(4) memory disks register as DEVSTAT_TYPE_DIRECT too (tmpmfs: a
 * swap-backed /tmp wrote 500 MB/s of "disk" I/O; a vnode-backed image,
 * whose I/O the disk under it counts already, as Linux's loop), and so
 * do CTL's block-backend LUNs (cbb: an iSCSI target's, whose I/O the
 * file's disk counts again). Neither is a disk here. */
int budyk_freebsd_disk_counted(const char* device_name) {
    return strcmp(device_name, "md") != 0 && strcmp(device_name, "cbb") != 0;
}

/* Per device (name = device_name + unit_number, as iostat): counters
 * c[] = bytes read, bytes written, reads, writes, microseconds busy
 * (busy_time, a bintime). */
int budyk_collect_disk_freebsd_ex(budyk_disk_ctx_c* ctx, budyk_sample_c* s, budyk_detail_c* detail) {
    if (ctx == NULL || s == NULL) return -EINVAL;
    budyk_entity_prev_c next[BUDYK_MAX_ENTITIES];
    uint32_t nnext = 0;
    const uint64_t ddns = detail != NULL && detail->disk_prev_ns != 0 &&
                          s->timestamp_nanos > detail->disk_prev_ns
                        ? s->timestamp_nanos - detail->disk_prev_ns : 0;
    if (detail != NULL) detail->ndisks = 0;

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

    budyk_counted_c counted[BUDYK_MAX_COUNTED] = {{"", 0, 0}};
    uint32_t ncounted    = 0;
    uint32_t counted_pos = 0;
    uint64_t read_moved  = 0;
    uint64_t write_moved = 0;
    uint32_t devs        = 0;
    for (int i = 0; i < stats.dinfo->numdevs; ++i) {
        const struct devstat* d = &stats.dinfo->devices[i];
        if ((d->device_type & DEVSTAT_TYPE_MASK) != DEVSTAT_TYPE_DIRECT) continue;
        if (d->device_type & DEVSTAT_TYPE_PASS) continue;
        if (d->device_name[0] == '\0') continue;
        if (!budyk_freebsd_disk_counted(d->device_name)) continue;
        char name[32];
        snprintf(name, sizeof(name), "%.20s%d", d->device_name, d->unit_number);
        budyk_counted_add(ctx->dev, ctx->ndev, &counted_pos, counted, &ncounted, name,
                          (uint64_t)d->bytes[DEVSTAT_READ], (uint64_t)d->bytes[DEVSTAT_WRITE],
                          &read_moved, &write_moved);
        ++devs;

        if (detail != NULL && detail->ndisks < BUDYK_MAX_ENTITIES) {
            const uint64_t busy_us = (uint64_t)d->busy_time.sec * 1000000ULL +
                                     (((uint64_t)(d->busy_time.frac >> 32) * 1000000ULL) >> 32);
            const uint64_t now[5] = {
                (uint64_t)d->bytes[DEVSTAT_READ], (uint64_t)d->bytes[DEVSTAT_WRITE],
                (uint64_t)d->operations[DEVSTAT_READ], (uint64_t)d->operations[DEVSTAT_WRITE],
                busy_us,
            };
            const budyk_entity_prev_c* p =
                budyk_find_prev(detail->disk_prev, detail->ndisk_prev, name);
            budyk_disk_dev_c* dev = &detail->disks[detail->ndisks++];
            memset(dev, 0, sizeof(*dev));
            snprintf(dev->name, sizeof(dev->name), "%s", name);
            if (p != NULL && ddns > 0) {
                dev->read_bytes_per_sec  = budyk_rate_u64(now[0], &p->c[0], ddns);
                dev->write_bytes_per_sec = budyk_rate_u64(now[1], &p->c[1], ddns);
                dev->reads_per_sec       = budyk_rate(now[2], &p->c[2], ddns);
                dev->writes_per_sec      = budyk_rate(now[3], &p->c[3], ddns);
                /* microseconds busy per second / 10^4 = percent */
                dev->busy_percent        = budyk_rate(now[4], &p->c[4], ddns) / 1.0e4;
                if (dev->busy_percent > 100.0) dev->busy_percent = 100.0;
            }
            budyk_entity_prev_c* n = &next[nnext++];
            memset(n, 0, sizeof(*n));
            snprintf(n->name, sizeof(n->name), "%s", name);
            memcpy(n->c, now, sizeof(now));
        }
    }
    if (detail != NULL) {
        memcpy(detail->disk_prev, next, sizeof(next[0]) * nnext);
        detail->ndisk_prev   = nnext;
        detail->disk_prev_ns = s->timestamp_nanos;
    }

    /* dinfo->mem_ptr is owned by devstat — free it before our calloc. */
    if (stats.dinfo->mem_ptr != NULL) free(stats.dinfo->mem_ptr);
    free(stats.dinfo);

    s->disk.device_count       = devs;
    s->disk.read_bytes_per_sec = 0;
    s->disk.write_bytes_per_sec = 0;

    if (ctx->has_prev) {
        const uint64_t now_ns = s->timestamp_nanos;
        const uint64_t dt_ns  = (now_ns > ctx->prev_ns) ? (now_ns - ctx->prev_ns) : 0;
        if (dt_ns > 0) {
            /* Each disk's own delta: one that comes back after a
             * hot-unplug starts again from a baseline. */
            const uint64_t zero = 0;
            s->disk.read_bytes_per_sec  = budyk_rate_u64(read_moved,  &zero, dt_ns);
            s->disk.write_bytes_per_sec = budyk_rate_u64(write_moved, &zero, dt_ns);
        }
    }

    memcpy(ctx->dev, counted, sizeof(counted[0]) * ncounted);
    ctx->ndev               = ncounted;
    ctx->prev_ns            = s->timestamp_nanos;
    ctx->has_prev           = 1;
    return 0;
}
