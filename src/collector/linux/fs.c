/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/fs.c — local filesystem usage: /proc/self/mounts,
 * then statvfs(3) on each mount point that is worth watching. */

#include "core/sample_c.h"

#include <sys/stat.h>
#include <sys/statvfs.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Not worth watching: kernel pseudo filesystems, images that are always
 * full (squashfs snaps, iso9660), overlay (a container's view of a
 * filesystem counted already), and network or FUSE filesystems, whose
 * statvfs() can block on a server that stopped answering: the
 * collector tick must not wait (CLAUDE.md). Read-only mounts are
 * skipped too: they do not fill up. */
static int skipped_type(const char* t) {
    static const char* kSkip[] = {
        "autofs", "binfmt_misc", "bpf", "cgroup", "cgroup2", "configfs",
        "debugfs", "devpts", "devtmpfs", "efivarfs", "fusectl", "hugetlbfs",
        "iso9660", "mqueue", "nsfs", "overlay", "proc", "pstore", "ramfs",
        "rpc_pipefs", "securityfs", "selinuxfs", "squashfs", "sysfs",
        "tracefs", "nfs", "nfs4", "cifs", "smb3", "smbfs", "ncpfs", "afs",
        "ceph", "glusterfs", "9p", "lustre", "gpfs", "sshfs",
    };
    for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); ++i) {
        if (strcmp(t, kSkip[i]) == 0) return 1;
    }
    return strncmp(t, "fuse", 4) == 0;
}

static int read_only(const char* opts) {
    return strcmp(opts, "ro") == 0 || strncmp(opts, "ro,", 3) == 0;
}

/* Mount points in /proc/self/mounts escape space, tab, newline and
 * backslash as \040, \011, \012, \134. */
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

    char path[512];
    snprintf(path, sizeof(path), "%s/self/mounts", budyk_linux_proc_root());
    FILE* f = fopen(path, "re");
    if (f == NULL) return -errno;

    dev_t    seen[64];
    size_t   nseen = 0;
    char     line[4096];
    while (fgets(line, sizeof(line), f) != NULL) {
        char dev[1024], mnt[1024], type[64], opts[1024];
        if (sscanf(line, "%1023s %1023s %63s %1023s", dev, mnt, type, opts) != 4) continue;
        if (skipped_type(type) || read_only(opts)) continue;
        budyk_linux_unescape_mount(mnt);

        /* The same filesystem mounted twice (a bind mount) counts once. */
        struct stat st;
        if (stat(mnt, &st) != 0) continue;
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
