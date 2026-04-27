/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/disk.c — aggregate disk throughput via devstat(3). */

#include "core/sample_c.h"

#include <sys/types.h>

#include <devstat.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* devstat_getdevs(3) returns the kernel's per-device statistics array.
 * struct devstat carries .bytes[DEVSTAT_READ] and .bytes[DEVSTAT_WRITE]
 * as monotonic 64-bit counters; we sum them across "real" disks and let
 * the caller-supplied timestamp drive the bytes-per-second rate.
 *
 * Filtering: skip cd*/pass*/fd* (optical / SCSI passthrough / floppy).
 *
 * Memory pattern matches iostat(8): a stack-resident struct devinfo
 * pre-zeroed, then handed to devstat_getdevs() which fills it in. The
 * library owns dinfo->mem_ptr across calls — for our one-shot per
 * sample tick we deliberately don't free it; the libdevstat pool is
 * reused on the next call and released only when libdevstat tears
 * itself down at process exit.
 */

static int is_skipped(const char* dev_name) {
    if (strncmp(dev_name, "cd",   2) == 0) return 1;
    if (strncmp(dev_name, "pass", 4) == 0) return 1;
    if (strncmp(dev_name, "fd",   2) == 0) return 1;
    return 0;
}

int budyk_collect_disk_freebsd(budyk_disk_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;

    struct devinfo  dinfo;
    struct statinfo cur;
    memset(&dinfo, 0, sizeof(dinfo));
    memset(&cur,   0, sizeof(cur));
    cur.dinfo = &dinfo;

    if (devstat_getdevs(NULL, &cur) < 0) return -EIO;

    uint64_t read_bytes  = 0;
    uint64_t write_bytes = 0;
    uint32_t device_count = 0;

    for (int i = 0; i < dinfo.numdevs; ++i) {
        const struct devstat* ds = &dinfo.devices[i];
        if (is_skipped(ds->device_name)) continue;
        read_bytes  += (uint64_t)ds->bytes[DEVSTAT_READ];
        write_bytes += (uint64_t)ds->bytes[DEVSTAT_WRITE];
        ++device_count;
    }

    s->disk.device_count = device_count;

    if (!ctx->has_prev || s->timestamp_nanos <= ctx->prev_ns) {
        s->disk.read_bytes_per_sec  = 0;
        s->disk.write_bytes_per_sec = 0;
    } else {
        uint64_t dr  = read_bytes  >= ctx->prev_read_sectors
                     ? read_bytes  - ctx->prev_read_sectors  : 0;
        uint64_t dw  = write_bytes >= ctx->prev_write_sectors
                     ? write_bytes - ctx->prev_write_sectors : 0;
        uint64_t dns = s->timestamp_nanos - ctx->prev_ns;
        /* devstat counters are already in bytes — no sector-size factor.
         * The ctx field is named after Linux's sector-counter heritage
         * but we just store the byte total here.
         */
        s->disk.read_bytes_per_sec  = (dr * 1000000000ULL) / dns;
        s->disk.write_bytes_per_sec = (dw * 1000000000ULL) / dns;
    }

    ctx->prev_read_sectors  = read_bytes;
    ctx->prev_write_sectors = write_bytes;
    ctx->prev_ns            = s->timestamp_nanos;
    ctx->has_prev           = 1;
    return 0;
}
