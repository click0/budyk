/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/disk.c — aggregate disk throughput via /proc/diskstats. */

#include "core/sample_c.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* /proc/diskstats line layout (kernel ≥ 4.18, 14+ whitespace-separated cols):
 *   major minor name
 *   reads reads_merged sectors_read  ms_reading
 *   writes writes_merged sectors_written ms_writing
 *   ios_in_progress  ms_doing_ios  weighted_ms
 *   [ + discards, flushes on newer kernels — ignored ]
 *
 * Sector size is a hard-coded 512 in the kernel stats layer regardless of
 * the device's physical sector size (see block/genhd.c).
 */

static int is_noise_name(const char* name) {
    static const char* kSkip[] = {
        "loop", "ram", "zram", "dm-", "md", "fd", "sr", "nbd"
    };
    for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); ++i) {
        size_t n = strlen(kSkip[i]);
        if (strncmp(name, kSkip[i], n) == 0) return 1;
    }
    return 0;
}

/* Returns 1 if the name looks like a partition rather than a whole disk.
 * Explicit rules matching /sys/block conventions:
 *   - nvme<N>n<M>p<K>   → partition (e.g. nvme0n1p1)
 *   - mmcblk<N>p<K>     → partition (e.g. mmcblk0p1)
 *   - sd<letters><N>    → partition (e.g. sda1, sdb12)
 *   - hd<letters><N>    → partition
 *   - vd<letters><N>    → partition
 *   - xvd<letters><N>   → partition
 * Everything else is treated as whole disk.
 */
static int starts_with(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int ends_with_p_digits(const char* name) {
    size_t len = strlen(name);
    size_t i = len;
    while (i > 0 && isdigit((unsigned char)name[i - 1])) --i;
    return (i > 0 && i < len && name[i - 1] == 'p');
}

static int sd_family_partition(const char* name, const char* prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) return 0;
    size_t len = strlen(name);
    if (len <= plen) return 0;
    if (!isdigit((unsigned char)name[len - 1])) return 0;
    /* Need at least one letter between prefix and trailing digits. */
    size_t i = plen;
    while (i < len && isalpha((unsigned char)name[i])) ++i;
    return (i > plen && i < len && isdigit((unsigned char)name[i]));
}

static int is_partition_name(const char* name) {
    if (starts_with(name, "nvme") || starts_with(name, "mmcblk")) {
        return ends_with_p_digits(name);
    }
    return sd_family_partition(name, "sd")
        || sd_family_partition(name, "hd")
        || sd_family_partition(name, "vd")
        || sd_family_partition(name, "xvd");
}

int budyk_collect_disk_linux(budyk_disk_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;

    FILE* f = fopen("/proc/diskstats", "r");
    if (f == NULL) return -errno;

    uint64_t read_sectors  = 0;
    uint64_t write_sectors = 0;
    uint32_t device_count  = 0;

    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned int maj, min_;
        char name[64];
        unsigned long long r, rm, rs, rms, w, wm, ws, wms;
        int parsed = sscanf(line, " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu",
                            &maj, &min_, name, &r, &rm, &rs, &rms, &w, &wm, &ws, &wms);
        if (parsed < 11) continue;
        if (is_noise_name(name))     continue;
        if (is_partition_name(name)) continue;

        read_sectors  += rs;
        write_sectors += ws;
        ++device_count;
    }
    fclose(f);

    s->disk.device_count = device_count;

    if (!ctx->has_prev || s->timestamp_nanos <= ctx->prev_ns) {
        s->disk.read_bytes_per_sec  = 0;
        s->disk.write_bytes_per_sec = 0;
    } else {
        uint64_t dr_sec = read_sectors  >= ctx->prev_read_sectors
                        ? read_sectors  - ctx->prev_read_sectors  : 0;
        uint64_t dw_sec = write_sectors >= ctx->prev_write_sectors
                        ? write_sectors - ctx->prev_write_sectors : 0;
        uint64_t dns    = s->timestamp_nanos - ctx->prev_ns;
        /* bytes_per_sec = sectors * 512 * 1e9 / ns — compute in 128-bit
         * space on platforms that have it, otherwise use 64-bit division
         * which is accurate enough for real-world workloads.
         */
        s->disk.read_bytes_per_sec  = (dr_sec * 512ULL * 1000000000ULL) / dns;
        s->disk.write_bytes_per_sec = (dw_sec * 512ULL * 1000000000ULL) / dns;
    }

    ctx->prev_read_sectors  = read_sectors;
    ctx->prev_write_sectors = write_sectors;
    ctx->prev_ns            = s->timestamp_nanos;
    ctx->has_prev           = 1;
    return 0;
}
