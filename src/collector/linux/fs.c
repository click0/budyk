/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/fs.c — local filesystem usage: /proc/self/mountinfo,
 * then statvfs(3) on each mount point that is worth watching. */

#include "core/sample_c.h"

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not worth watching: kernel pseudo filesystems, images that are always
 * full (squashfs snaps, iso9660), overlay (a container's view of a
 * filesystem counted already), and network or FUSE filesystems, whose
 * statvfs() can block on a server that stopped answering: the
 * collector tick must not wait (CLAUDE.md). A read-only filesystem is
 * skipped too: it does not fill up. */
static int skipped_type(const char* t) {
    static const char* kSkip[] = {
        "autofs", "binfmt_misc", "bpf", "cgroup", "cgroup2", "configfs",
        "debugfs", "devpts", "devtmpfs", "efivarfs", "fusectl", "hugetlbfs",
        "iso9660", "mqueue", "nsfs", "overlay", "proc", "pstore", "ramfs",
        "rpc_pipefs", "securityfs", "selinuxfs", "squashfs", "sysfs",
        "tracefs", "nfs", "nfs4", "cifs", "smb3", "smbfs", "ncpfs", "afs",
        "ceph", "glusterfs", "9p", "lustre", "gpfs", "sshfs",
        /* statfs() waits on a daemon or a cluster lock too: virtiofs on
         * the host's virtiofsd, orangefs and coda on their clients, ocfs2
         * and gfs2 on the DLM (a fencing held the tick), beegfs on its
         * servers. */
        "virtiofs", "orangefs", "coda", "ocfs2", "gfs2", "beegfs",
    };
    for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); ++i) {
        if (strcmp(t, kSkip[i]) == 0) return 1;
    }
    return strncmp(t, "fuse", 4) == 0;
}

/* Whether a comma-separated option list says "ro". */
static int read_only(const char* opts) {
    return strcmp(opts, "ro") == 0 || strncmp(opts, "ro,", 3) == 0;
}

/* One line of /proc/self/mountinfo:
 *   36 35 98:0 /root /mnt rw,noatime master:1 - ext4 /dev/sda1 rw,errors=continue
 * the mount point (field 5), and after the " - " the type, the source
 * and the filesystem's own options. 0 when the line does not read. */
static int parse_mountinfo(char* line, char* mnt, char* type, char* dev, char* sopts) {
    char* sep = strstr(line, " - ");
    if (sep == NULL) return 0;
    *sep = '\0';
    return sscanf(line, "%*s %*s %*s %*s %1023s", mnt) == 1 &&
           sscanf(sep + 3, "%63s %1023s %1023s", type, dev, sopts) == 3;
}

/* FNV-1a of a mount point, for the "mounted over" check below. */
static uint64_t hash_str(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s != '\0'; ++s) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

/* The mounts of the first pass: each line's mount id, its parent's, and
 * its mount point, for the "hidden" check below. A filesystem another is
 * mounted on at the same point is hidden, and what the point names now
 * is the top one (stat() and statvfs() of the hidden line gave the top's
 * numbers under the hidden one's name and type: /dev/shm mounted twice,
 * an image mounted over a directory). Hashes, not names: a node with
 * thousands of mounts is a few hundred KB here. Past the array a line is
 * taken as it is. */
#define BUDYK_FS_MAX_LINES 8192
static struct {
    unsigned      id, parent;
    uint64_t      point;    /* 0: the line did not read */
    int32_t       up;       /* the parent's index here, -1 if it is not listed */
    unsigned char hidden;
} g_mounts[BUDYK_FS_MAX_LINES];
static uint32_t g_by_id[BUDYK_FS_MAX_LINES];   /* g_mounts indices, by id */
static size_t   g_nmounts;

static int by_id_cmp(const void* a, const void* b) {
    const unsigned x = g_mounts[*(const uint32_t*)a].id, y = g_mounts[*(const uint32_t*)b].id;
    return x < y ? -1 : x > y;
}

/* The g_mounts index of mount `id`, -1 if the first pass did not list it. */
static int32_t find_mount(unsigned id) {
    size_t lo = 0, hi = g_nmounts;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const unsigned m = g_mounts[g_by_id[mid]].id;
        if (m == id) return (int32_t)g_by_id[mid];
        if (m < id) lo = mid + 1; else hi = mid;
    }
    return -1;
}

/* By the mount tree, not by the lines' order or number: hidden is a
 * mount whose child is mounted at the same point, and every mount under
 * a hidden one (its point is reached through the top one). The order of
 * the lines said it before: after `mount --move` an older mount can be
 * listed before the newer one it was moved onto, and the moved one, on
 * top, was the one left out. And the second read took line k's verdict
 * from the first read's line k: a mount removed between them gave every
 * later line its neighbour's (a filesystem missing from a tick, or one
 * covered counted, its statvfs() reaching the server covering it). */
static void mark_hidden(void) {
    for (size_t i = 0; i < g_nmounts; ++i) g_by_id[i] = (uint32_t)i;
    qsort(g_by_id, g_nmounts, sizeof(g_by_id[0]), by_id_cmp);
    for (size_t i = 0; i < g_nmounts; ++i) {
        g_mounts[i].hidden = 0;
        const int32_t up = find_mount(g_mounts[i].parent);
        g_mounts[i].up = up >= 0 && (size_t)up != i ? up : -1;
    }
    for (size_t i = 0; i < g_nmounts; ++i) {
        const int32_t up = g_mounts[i].up;
        if (up >= 0 && g_mounts[i].point != 0 && g_mounts[up].point == g_mounts[i].point) {
            g_mounts[up].hidden = 1;
        }
    }
    /* Down the tree, a level a round (mounts nest a few deep), to the
     * mounts under a hidden one; not to the one on it at its own point,
     * which is what hides it. */
    for (int round = 0; round < 64; ++round) {
        int changed = 0;
        for (size_t i = 0; i < g_nmounts; ++i) {
            const int32_t up = g_mounts[i].up;
            if (!g_mounts[i].hidden && up >= 0 && g_mounts[up].hidden &&
                g_mounts[up].point != g_mounts[i].point) {
                g_mounts[i].hidden = 1;
                changed = 1;
            }
        }
        if (!changed) break;
    }
}

/* The autofs mounts of the first pass (their mount id and point): a
 * filesystem mounted on one is an automount, left out. Each stat() and
 * statvfs() of its point counts as a use (autofs_d_manage()), so its idle
 * timer never ran out and it stayed mounted for ever (a backup disk with
 * x-systemd.idle-timeout, the ESP). */
#define BUDYK_FS_MAX_AUTOFS 256
static struct { unsigned id; uint64_t point; } g_autofs[BUDYK_FS_MAX_AUTOFS];

static int on_autofs(unsigned parent, uint64_t point, size_t nautofs) {
    for (size_t i = 0; i < nautofs; ++i) {
        if (g_autofs[i].id == parent && g_autofs[i].point == point) return 1;
    }
    return 0;
}

/* Mount points in /proc/self/mounts and mountinfo escape space, tab,
 * newline and backslash as \040, \011, \012, \134. */
void budyk_linux_unescape_mount(char* s) {
    char* out = s;
    for (const char* p = s; *p != '\0'; ++p) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' &&
            p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
            *out++ = (char)(((p[1] - '0') << 6) | ((p[2] - '0') << 3) | (p[3] - '0'));
            p += 3;
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
}

int budyk_collect_fs_linux(budyk_sample_c* s) {
    return budyk_collect_fs_linux_ex(s, NULL);
}

int budyk_collect_fs_linux_ex(budyk_sample_c* s, budyk_detail_c* d) {
    if (s == NULL) return -EINVAL;
    memset(&s->fs, 0, sizeof(s->fs));
    if (d != NULL) d->nmounts = 0;

    /* mountinfo, not mounts: the read-only test must be the filesystem's
     * own ("ro" after the " - "). The mount's options are this mount
     * namespace's view, and under systemd's ProtectSystem=strict (the
     * packaged unit) every mount but the data directory reads "ro" there:
     * /, /srv, /var/lib/... were skipped and never reported full. */
    char path[512];
    snprintf(path, sizeof(path), "%s/self/mountinfo", budyk_linux_proc_root());
    FILE* f = fopen(path, "re");
    if (f == NULL) return -errno;

    /* First pass: every mount's id, parent and point, for mark_hidden(),
     * and the autofs mounts, for on_autofs(). */
    size_t nautofs = 0;
    char   line[4096];
    g_nmounts = 0;
    while (g_nmounts < BUDYK_FS_MAX_LINES && fgets(line, sizeof(line), f) != NULL) {
        char dev[1024], mnt[1024], type[64], opts[1024];
        unsigned id = 0, parent = 0;
        if (sscanf(line, "%u %u", &id, &parent) != 2) continue;
        const int ok = parse_mountinfo(line, mnt, type, dev, opts);
        g_mounts[g_nmounts].id     = id;
        g_mounts[g_nmounts].parent = parent;
        g_mounts[g_nmounts].point  = ok ? hash_str(mnt) : 0;
        ++g_nmounts;
        if (ok && strcmp(type, "autofs") == 0 && nautofs < BUDYK_FS_MAX_AUTOFS) {
            g_autofs[nautofs].id    = id;
            g_autofs[nautofs].point = hash_str(mnt);
            ++nautofs;
        }
    }
    mark_hidden();
    if (fseek(f, 0, SEEK_SET) != 0) {
        const int e = errno;
        fclose(f);
        return -e;
    }

    dev_t    seen[256];
    size_t   nseen = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char dev[1024], mnt[1024], type[64], opts[1024];
        unsigned id = 0, parent = 0;
        const int ids = sscanf(line, "%u %u", &id, &parent);
        if (!parse_mountinfo(line, mnt, type, dev, opts)) continue;
        /* The verdict by mount id: one listed since the first read is taken. */
        const int32_t at = ids == 2 ? find_mount(id) : -1;
        if (at >= 0 && g_mounts[at].hidden) continue;
        if (ids == 2 && nautofs > 0 && on_autofs(parent, hash_str(mnt), nautofs)) continue;
        if (skipped_type(type) || read_only(opts)) continue;
        budyk_linux_unescape_mount(mnt);
        budyk_linux_unescape_mount(dev);

        /* A mount point budyk cannot reach (EACCES: under a 0700 home,
         * /root, as the packaged unit's user) is counted for the log; it
         * was skipped in silence, a full one never reported. */
        struct stat st;
        if (stat(mnt, &st) != 0) {
            if (errno == EACCES) ++s->fs.unreadable;
            continue;
        }
        /* The same filesystem mounted twice (a bind mount) counts once. */
        int dup = 0;
        for (size_t i = 0; i < nseen; ++i) dup |= seen[i] == st.st_dev;
        if (dup) continue;
        if (nseen < sizeof(seen) / sizeof(seen[0])) seen[nseen++] = st.st_dev;

        struct statvfs v;
        if (statvfs(mnt, &v) != 0 || v.f_blocks == 0) continue;
        /* As df(1): used / (used + available to unprivileged users). */
        const double used  = (double)(v.f_blocks - v.f_bfree);
        const double avail = (double)v.f_bavail;
        const double pct   = used + avail > 0.0 ? used * 100.0 / (used + avail) : 0.0;
        const double ipct  = v.f_files > 0
            ? (double)(v.f_files - v.f_ffree) * 100.0 / (double)v.f_files : 0.0;
        ++s->fs.count;
        if (pct  > s->fs.max_used_percent)        s->fs.max_used_percent        = pct;
        if (ipct > s->fs.max_inodes_used_percent) s->fs.max_inodes_used_percent = ipct;

        if (d != NULL && d->nmounts < BUDYK_MAX_ENTITIES) {
            budyk_mount_c* m = &d->mounts[d->nmounts++];
            memset(m, 0, sizeof(*m));
            snprintf(m->mount,  sizeof(m->mount),  "%.255s", mnt);
            snprintf(m->type,   sizeof(m->type),   "%.31s",  type);
            snprintf(m->device, sizeof(m->device), "%.127s", dev);
            const uint64_t frsize = v.f_frsize > 0 ? (uint64_t)v.f_frsize : (uint64_t)v.f_bsize;
            m->total_bytes         = (uint64_t)v.f_blocks * frsize;
            m->used_bytes          = (uint64_t)(v.f_blocks - v.f_bfree) * frsize;
            m->avail_bytes         = (uint64_t)v.f_bavail * frsize;
            m->used_percent        = pct;
            m->inodes_total        = (uint64_t)v.f_files;
            m->inodes_used         = (uint64_t)(v.f_files - v.f_ffree);
            m->inodes_used_percent = ipct;
        }
    }
    fclose(f);
    s->fs.present = 1;
    return 0;
}
