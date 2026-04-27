/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/disk.c — aggregate disk throughput via devstat(3). */

#include "core/sample_c.h"

#include <devstat.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* devstat_getdevs(3) returns the kernel's per-device statistics array.
 * struct devstat carries .bytes[DEVSTAT_READ] and .bytes[DEVSTAT_WRITE]
 * as monotonic 64-bit counters; we sum them across "real" disks and let
 * the caller-supplied timestamp drive the bytes-per-second rate.
 *
 * Filtering: skip cd*/pass*/fd* (optical / SCSI passthrough / floppy).
 * Everything else — ada*, da*, nvd*, nvme*, mmcsd*, vtbd*, gpt/zvol — is
 * counted. Whole-disk vs slice/partition distinction is moot here:
 * GEOM exposes byte counters at the leaf nodes.
 */

static int is_skipped(const char* dev_name) {
    if (strncmp(dev_name, "cd",   2) == 0) return 1;
    if (strncmp(dev_name, "pass", 4) == 0) return 1;
    if (strncmp(dev_name, "fd",   2) == 0) return 1;
    return 0;
}

int budyk_collect_disk_freebsd(budyk_disk_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;

    if (devstat_checkversion(NULL) < 0) return -EIO;

    struct statinfo cur;
    memset(&cur, 0, sizeof(cur));
    cur.dinfo = (struct devinfo*)calloc(1, sizeof(struct devinfo));
    if (cur.dinfo == NULL) return -ENOMEM;

    if (devstat_getdevs(NULL, &cur) < 0) {
        free(cur.dinfo);
        return -EIO;
    }

    uint64_t read_bytes  = 0;
    uint64_t write_bytes = 0;
    uint32_t device_count = 0;

    for (int i = 0; i < cur.dinfo->numdevs; ++i) {
        const struct devstat* ds = &cur.dinfo->devices[i];
        if (is_skipped(ds->device_name)) continue;

        read_bytes  += (uint64_t)ds->bytes[DEVSTAT_READ];
        write_bytes += (uint64_t)ds->bytes[DEVSTAT_WRITE];
        ++device_count;
    }

    /* devstat_getdevs allocates an internal block referenced by
     * dinfo->mem_ptr — release it before freeing dinfo itself.
     */
    if (cur.dinfo->mem_ptr != NULL) free(cur.dinfo->mem_ptr);
    free(cur.dinfo);

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
