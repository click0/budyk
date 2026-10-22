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
    return budyk_collect_disk_linux_ex(ctx, s, NULL);
}

/* Per device: counters c[] = bytes read, bytes written, reads, writes,
 * ms with I/O in flight (io_ticks, field 13). busy_percent is that
 * time's share of the interval. */
int budyk_collect_disk_linux_ex(budyk_disk_ctx_c* ctx, budyk_sample_c* s, budyk_detail_c* d) {
    if (ctx == NULL || s == NULL) return -EINVAL;
    budyk_entity_prev_c next[BUDYK_MAX_ENTITIES];
    uint32_t nnext = 0;
    const uint64_t ddns = d != NULL && d->disk_prev_ns != 0 && s->timestamp_nanos > d->disk_prev_ns
                       ? s->timestamp_nanos - d->disk_prev_ns : 0;
    if (d != NULL) d->ndisks = 0;

    char path[512];
    snprintf(path, sizeof(path), "%s/diskstats", budyk_linux_proc_root());
    FILE* f = fopen(path, "re");
    if (f == NULL) return -errno;

    uint64_t read_sectors  = 0;
    uint64_t write_sectors = 0;
    uint32_t device_count  = 0;

    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned int maj, min_;
        char name[64];
        unsigned long long r, rm, rs, rms, w, wm, ws, wms, inflight = 0, ticks = 0;
        int parsed = sscanf(line, " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &maj, &min_, name, &r, &rm, &rs, &rms, &w, &wm, &ws, &wms,
                            &inflight, &ticks);
        if (parsed < 11) continue;
        if (is_noise_name(name))     continue;
        if (is_partition_name(name)) continue;

        read_sectors  += rs;
        write_sectors += ws;
        ++device_count;

        if (d != NULL && d->ndisks < BUDYK_MAX_ENTITIES) {
            const uint64_t now[5] = { rs * 512ULL, ws * 512ULL, r, w, ticks };
            const budyk_entity_prev_c* p = budyk_find_prev(d->disk_prev, d->ndisk_prev, name);
            budyk_disk_dev_c* dev = &d->disks[d->ndisks++];
            memset(dev, 0, sizeof(*dev));
            snprintf(dev->name, sizeof(dev->name), "%.31s", name);
            if (p != NULL && ddns > 0) {
                dev->read_bytes_per_sec  = budyk_rate_u64(now[0], &p->c[0], ddns);
                dev->write_bytes_per_sec = budyk_rate_u64(now[1], &p->c[1], ddns);
                dev->reads_per_sec       = budyk_rate(now[2], &p->c[2], ddns);
                dev->writes_per_sec      = budyk_rate(now[3], &p->c[3], ddns);
                /* io_ticks is in ms: ms per s / 10 = percent */
                dev->busy_percent        = budyk_rate(now[4], &p->c[4], ddns) / 10.0;
                if (dev->busy_percent > 100.0) dev->busy_percent = 100.0;
            }
            budyk_entity_prev_c* n = &next[nnext++];
            memset(n, 0, sizeof(*n));
            snprintf(n->name, sizeof(n->name), "%.31s", name);
            memcpy(n->c, now, sizeof(now));
        }
    }
    fclose(f);

    s->disk.device_count = device_count;

    if (!ctx->has_prev || s->timestamp_nanos <= ctx->prev_ns) {
        s->disk.read_bytes_per_sec  = 0;
        s->disk.write_bytes_per_sec = 0;
    } else {
        /* Sectors of 512 bytes; a counter that went back reads 0. */
        const uint64_t dns = s->timestamp_nanos - ctx->prev_ns;
        const uint64_t prev_read  = ctx->prev_read_sectors  * 512ULL;
        const uint64_t prev_write = ctx->prev_write_sectors * 512ULL;
        s->disk.read_bytes_per_sec  = budyk_rate_u64(read_sectors  * 512ULL, &prev_read,  dns);
        s->disk.write_bytes_per_sec = budyk_rate_u64(write_sectors * 512ULL, &prev_write, dns);
    }

    if (d != NULL) {
        memcpy(d->disk_prev, next, sizeof(next[0]) * nnext);
        d->ndisk_prev   = nnext;
        d->disk_prev_ns = s->timestamp_nanos;
    }
    ctx->prev_read_sectors  = read_sectors;
    ctx->prev_write_sectors = write_sectors;
    ctx->prev_ns            = s->timestamp_nanos;
    ctx->has_prev           = 1;
    return 0;
}
