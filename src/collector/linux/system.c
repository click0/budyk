/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/system.c — uptime, load, entropy, processes, clock sync,
 * self-metrics and thermal. */

#include "core/sample_c.h"

#include <sys/resource.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <unistd.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int budyk_collect_uptime_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    FILE* f = fopen("/proc/uptime", "re");
    if (f == NULL) return -errno;

    double up = 0.0;
    int n = fscanf(f, "%lf", &up);
    fclose(f);
    if (n != 1) return -EIO;

    s->uptime_seconds = up;
    return 0;
}

int budyk_collect_load_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    double avg[3] = {0.0, 0.0, 0.0};
    int n = getloadavg(avg, 3);
    if (n != 3) return -EIO;

    s->load.avg_1m  = avg[0];
    s->load.avg_5m  = avg[1];
    s->load.avg_15m = avg[2];
    return 0;
}

/* /proc/sys/kernel/random/entropy_avail — single integer, the number
 * of bits currently in the kernel's CSPRNG entropy pool. Populated on
 * every Linux >= 2.6. On stripped containers the file may be missing
 * (read-only / unsupported sysctl) — soft-fail with present=0.
 */
int budyk_collect_entropy_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    FILE* f = fopen("/proc/sys/kernel/random/entropy_avail", "re");
    if (f == NULL) {
        s->entropy.available_bits = 0;
        s->entropy.present        = 0;
        return 0;                  /* missing on this host — non-fatal */
    }
    unsigned int bits = 0;
    int n = fscanf(f, "%u", &bits);
    fclose(f);
    if (n != 1) {
        s->entropy.available_bits = 0;
        s->entropy.present        = 0;
        return 0;
    }
    s->entropy.available_bits = bits;
    s->entropy.present        = 1;
    return 0;
}

/* Processes by state: one /proc/<pid>/stat per process.
 *   <pid> (<comm>) <state> <ppid> ... <num_threads (field 20)> ...
 * comm can hold spaces and parentheses, so the fields are counted from
 * the last ')'. States: R running; S, I (idle kernel thread), W, P
 * sleeping; D blocked (uninterruptible); T, t stopped; Z zombie; X (dead,
 * being reaped) is not counted. A process that exits between readdir()
 * and the read is skipped.
 */
int budyk_linux_parse_proc_stat(const char* line, char* state, uint32_t* threads) {
    budyk_proc_read_c r;
    uint32_t flags = 0;
    if (budyk_linux_parse_proc_stat_ex(line, &r, &flags) != 0) return -EINVAL;
    *state   = r.state;
    *threads = r.threads;
    return 0;
}

/* The whole line: pid (field 1), comm (2), state (3), flags (9), utime +
 * stime (14, 15) as `cpu` in clock ticks, num_threads (20), starttime
 * (22) as `start`, rss (24) in pages, as bytes. A line that ends after
 * field 20 reads 0 for the rest. */
int budyk_linux_parse_proc_stat_ex(const char* line, budyk_proc_read_c* out, uint32_t* flags) {
    const char* lp = strchr(line, '(');
    const char* rp = strrchr(line, ')');
    if (lp == NULL || rp == NULL || rp < lp || rp[1] != ' ' || rp[2] == '\0') return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->pid = (int32_t)strtol(line, NULL, 10);
    size_t len = (size_t)(rp - lp - 1);
    if (len >= sizeof(out->name)) len = sizeof(out->name) - 1;
    memcpy(out->name, lp + 1, len);
    out->state = rp[2];
    /* Fields 4..24 after the state; up to num_threads (20) required,
     * the rest 0 if a short line lacks them. */
    long long f[25];
    memset(f, 0, sizeof(f));
    const char* p = rp + 3;
    for (int field = 4; field <= 24; ++field) {
        char* end = NULL;
        f[field] = strtoll(p, &end, 10);
        if (end == p) {
            if (field <= 20) return -EINVAL;
            f[field] = 0;
            break;
        }
        p = end;
    }
    const long page = sysconf(_SC_PAGESIZE);
    *flags         = (uint32_t)f[9];
    out->cpu       = (uint64_t)(f[14] > 0 ? f[14] : 0) + (uint64_t)(f[15] > 0 ? f[15] : 0);
    out->threads   = (uint32_t)f[20];
    out->start     = (uint64_t)f[22];
    out->rss_bytes = f[24] > 0 ? (uint64_t)f[24] * (uint64_t)(page > 0 ? page : 4096) : 0;
    return 0;
}

#define BUDYK_PF_KTHREAD 0x00200000u   /* <linux/sched.h> PF_KTHREAD */

int budyk_collect_proc_linux(budyk_sample_c* s) {
    return budyk_collect_proc_linux_ex(s, NULL);
}

int budyk_collect_proc_linux_ex(budyk_sample_c* s, budyk_detail_c* d) {
    if (s == NULL) return -EINVAL;
    memset(&s->proc, 0, sizeof(s->proc));

    const char* root = budyk_linux_proc_root();
    DIR* dir = opendir(root);
    if (dir == NULL) return -errno;
    const long tck = sysconf(_SC_CLK_TCK);
    if (d != NULL) budyk_procs_begin(d, s->timestamp_nanos);
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '1' || ent->d_name[0] > '9') continue;
        char path[512];
        const int n = snprintf(path, sizeof(path), "%s/%s/stat", root, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;
        FILE* f = fopen(path, "re");
        if (f == NULL) continue;
        /* The whole file, not a line: the kernel prints the name as it
         * is, and a process named "x\n" (one prctl()) ended fgets() before
         * its ')': it went uncounted and out of the top list. */
        char line[1024];
        const size_t len = fread(line, 1, sizeof(line) - 1, f);
        line[len] = '\0';
        const int got = len > 0;
        fclose(f);
        budyk_proc_read_c r;
        uint32_t flags = 0;
        if (!got || budyk_linux_parse_proc_stat_ex(line, &r, &flags) != 0) continue;
        switch (r.state) {
            case 'R':                               ++s->proc.running;  break;
            case 'S': case 'I': case 'W': case 'P': ++s->proc.sleeping; break;
            case 'D': case 'K':                     ++s->proc.blocked;  break;   /* K: wakekill, 3.9-4.13 */
            case 'T': case 't':                     ++s->proc.stopped;  break;
            case 'Z':                               ++s->proc.zombie;   break;
            default:                                continue;   /* X, x: being reaped */
        }
        ++s->proc.total;
        s->proc.threads += r.threads;
        if (d != NULL && (flags & BUDYK_PF_KTHREAD) == 0) {
            budyk_procs_add(d, &r, (double)(tck > 0 ? tck : 100));
        }
    }
    closedir(dir);
    if (d != NULL) budyk_procs_end(d);
    /* /proc mounted hidepid=1 or 2 and budyk not root (the packaged
     * unit's user): readdir() lists budyk's own processes only, and
     * proc.total = 1 read as a reading, nothing logged. Pid 1 is always
     * there; one that cannot be read means the rest are hidden too, and
     * the group is left out, logged once ("Permission denied"). On pid 1
     * alone: any other process of budyk's user (an alert's curl, an
     * exec() command) is visible too, and the count of two was stored as
     * a reading. */
    if (strcmp(root, "/proc") == 0 && access("/proc/1/stat", R_OK) != 0) {
        return -EACCES;
    }
    return 0;
}

/* Clock sync: the kernel's NTP discipline, read with adjtimex(2) and no
 * mode bits (no privilege needed). Synchronised unless the kernel says
 * TIME_ERROR or STA_UNSYNC; the offset is in microseconds, or in
 * nanoseconds under STA_NANO; maxerror in microseconds.
 */
int budyk_collect_clock_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;
    memset(&s->clock, 0, sizeof(s->clock));
    struct timex tx;
    memset(&tx, 0, sizeof(tx));
    const int state = adjtimex(&tx);
    if (state < 0) return 0;          /* soft-fail: present stays 0 */
    s->clock.present      = 1;
    s->clock.synced       = (uint8_t)(state != TIME_ERROR && (tx.status & STA_UNSYNC) == 0);
    s->clock.offset_ms    = (tx.status & STA_NANO) != 0 ? (double)tx.offset / 1.0e6
                                                        : (double)tx.offset / 1.0e3;
    s->clock.max_error_ms = (double)tx.maxerror / 1.0e3;
    return 0;
}

/* Self-metrics — the daemon's own resource consumption.
 * Linux:
 *   * /proc/self/statm field 1 ("size" in pages) → vsz, but we surface
 *     RSS instead via field 2 ("resident" in pages); multiply by
 *     sysconf(_SC_PAGESIZE).
 *   * getrusage(RUSAGE_SELF) for ru_maxrss (peak, KiB) and the
 *     user/system CPU time accumulators.
 * The fields are best-effort — soft-fail to 0 on any failure.
 */
int budyk_collect_self_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    s->self_.rss_bytes          = 0;
    s->self_.peak_rss_bytes     = 0;
    s->self_.cpu_user_seconds   = 0.0;
    s->self_.cpu_system_seconds = 0.0;

    /* Current RSS. */
    FILE* f = fopen("/proc/self/statm", "re");
    if (f != NULL) {
        unsigned long long sz_pages = 0, rss_pages = 0;
        if (fscanf(f, "%llu %llu", &sz_pages, &rss_pages) == 2) {
            const long page = sysconf(_SC_PAGESIZE);
            if (page > 0) {
                s->self_.rss_bytes = (uint64_t)rss_pages * (uint64_t)page;
            }
        }
        fclose(f);
    }

    /* Peak RSS + CPU time. */
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        /* ru_maxrss: KiB on Linux. */
        s->self_.peak_rss_bytes     = (uint64_t)ru.ru_maxrss * 1024ULL;
        s->self_.cpu_user_seconds   =
            (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1.0e6;
        s->self_.cpu_system_seconds =
            (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1.0e6;
    }
    return 0;
}

/* One temperature file (millidegrees Celsius, 45000 = 45.0°C) into the
 * running hottest / count; a file that is not there or does not read
 * (a sensor with no reading, "Input/output error") is skipped, and so is
 * a reading no sensor makes: an input of a Super I/O chip (nct67xx,
 * it87) with nothing wired to it reads +127 or -128 °C (the ends of an
 * 8-bit reading) for ever, and a host read 127 °C for its life, any
 * real overheat under it. One that says so in temp<N>_fault is skipped
 * too (hwmon_fault()). */
static void thermal_read(const char* path, double* hottest, uint32_t* count) {
    FILE* f = fopen(path, "re");
    if (f == NULL) return;
    long milli = 0;
    const int ok = fscanf(f, "%ld", &milli);
    fclose(f);
    if (ok != 1) return;
    const double celsius = (double)milli / 1000.0;
    if (celsius >= 127.0 || celsius <= -60.0) return;
    if (celsius > *hottest) *hottest = celsius;
    ++*count;
}

/* Whether hwmon chip `chip` (its directory) is read: not drivetemp, whose
 * read is a SCSI command to the drive (it holds a disk spinning that was
 * set to spin down, and a disk that hangs holds the tick for up to a
 * minute), and not the thermal core's twin of a zone (its `device` is
 * the thermal_zone, read as a zone already: each ACPI zone counted
 * twice). */
static int hwmon_read(const char* chip) {
    char path[768], target[512];
    snprintf(path, sizeof(path), "%s/name", chip);
    FILE* f = fopen(path, "re");
    if (f != NULL) {
        char name[64] = "";
        const int ok = fscanf(f, "%63s", name);
        fclose(f);
        if (ok == 1 && strcmp(name, "drivetemp") == 0) return 0;
    }
    snprintf(path, sizeof(path), "%s/device", chip);
    const ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = '\0';
        const char* last = strrchr(target, '/');
        if (strncmp(last != NULL ? last + 1 : target, "thermal_zone", 12) == 0) return 0;
    }
    return 1;
}

/* Whether temp<N>_input's sensor says it is faulty (temp<N>_fault 1). */
static int hwmon_fault(const char* chip, const char* input) {
    char path[768];
    const size_t stem = strlen(input) - 6;              /* "temp3" of "temp3_input" */
    snprintf(path, sizeof(path), "%s/%.*s_fault", chip, (int)stem, input);
    FILE* f = fopen(path, "re");
    if (f == NULL) return 0;
    int fault = 0;
    if (fscanf(f, "%d", &fault) != 1) fault = 0;
    fclose(f);
    return fault == 1;
}

/* Thermal — the hottest reading across /sys/class/thermal/thermal_zone*\/
 * temp (ACPI zones: laptops, some servers) and /sys/class/hwmon/hwmon*\/
 * temp*_input (the chip drivers: k10temp, coretemp, nvme, IPMI; not
 * drivetemp, see hwmon_read()). Most servers have hwmon only, and read
 * "no sensor" from the zones alone. present=0 (sensor_count=0,
 * max_celsius=0.0) when neither is there (containers, jails, virt
 * without ACPI).
 */
int budyk_collect_thermal_linux(budyk_sample_c* s) {
    if (s == NULL) return -EINVAL;

    s->thermal.max_celsius  = 0.0;
    s->thermal.sensor_count = 0;
    s->thermal.present      = 0;

    double   hottest = -1e9;
    uint32_t count   = 0;
    char     dirpath[512];
    char     path[768];

    snprintf(dirpath, sizeof(dirpath), "%s/class/thermal", budyk_linux_sys_root());
    DIR* d = opendir(dirpath);
    if (d != NULL) {
        struct dirent* ent;
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(ent->d_name, "thermal_zone", 12) != 0) continue;
            const int n = snprintf(path, sizeof(path), "%s/%s/temp", dirpath, ent->d_name);
            if (n <= 0 || (size_t)n >= sizeof(path)) continue;
            thermal_read(path, &hottest, &count);
        }
        closedir(d);
    }

    snprintf(dirpath, sizeof(dirpath), "%s/class/hwmon", budyk_linux_sys_root());
    d = opendir(dirpath);
    if (d != NULL) {
        struct dirent* ent;
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(ent->d_name, "hwmon", 5) != 0) continue;
            char chip[640];
            const int n = snprintf(chip, sizeof(chip), "%s/%s", dirpath, ent->d_name);
            if (n <= 0 || (size_t)n >= sizeof(chip)) continue;
            if (!hwmon_read(chip)) continue;
            DIR* cd = opendir(chip);
            if (cd == NULL) continue;
            struct dirent* in;
            while ((in = readdir(cd)) != NULL) {
                const size_t len = strlen(in->d_name);
                if (strncmp(in->d_name, "temp", 4) != 0 || len < 11 ||
                    strcmp(in->d_name + len - 6, "_input") != 0) {
                    continue;
                }
                const int m = snprintf(path, sizeof(path), "%s/%s", chip, in->d_name);
                if (m <= 0 || (size_t)m >= sizeof(path)) continue;
                if (hwmon_fault(chip, in->d_name)) continue;
                thermal_read(path, &hottest, &count);
            }
            closedir(cd);
        }
        closedir(d);
    }

    if (count > 0) {
        s->thermal.max_celsius  = hottest;
        s->thermal.sensor_count = count;
        s->thermal.present      = 1;
    }
    return 0;
}
