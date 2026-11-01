/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/fs.c — local filesystem usage: the mount list from
 * getmntinfo(3) (MNT_NOWAIT: no filesystem is asked anything), then a
 * statfs(2) of each mount point worth watching for fresh numbers. */

#include "core/sample_c.h"

#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Not worth watching: pseudo filesystems, nullfs (another view of a
 * filesystem counted already), CD images, and network or FUSE
 * filesystems, whose statfs() can block on a server that stopped
 * answering: the collector tick must not wait. Read-only mounts are
 * skipped too: they do not fill up. */
static int skipped_type(const char* t) {
    static const char* kSkip[] = {
        "devfs", "procfs", "linprocfs", "linsysfs", "fdescfs", "autofs",
        "nullfs", "mqueuefs", "cd9660", "udf", "nfs", "oldnfs", "smbfs",
        "p9fs",
    };
    for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); ++i) {
        if (strcmp(t, kSkip[i]) == 0) return 1;
    }
    return strncmp(t, "fuse", 4) == 0;
}

int budyk_collect_fs_freebsd(budyk_sample_c* s) {
    return budyk_collect_fs_freebsd_ex(s, NULL);
}

int budyk_collect_fs_freebsd_ex(budyk_sample_c* s, budyk_detail_c* d) {
    if (s == NULL) return -EINVAL;
    memset(&s->fs, 0, sizeof(s->fs));
    if (d != NULL) d->nmounts = 0;

    struct statfs* mounts = NULL;
    const int n = getmntinfo(&mounts, MNT_NOWAIT);
    if (n <= 0) return -errno;
    for (int i = 0; i < n; ++i) {
        if ((mounts[i].f_flags & MNT_RDONLY) != 0) continue;
        if (skipped_type(mounts[i].f_fstypename)) continue;
        /* A mount point the daemon cannot reach (EACCES: a dataset or a
         * jail's root under a 0700 directory, and budyk is not root) was
         * left out without a word; the list's own numbers, as of
         * getmntinfo(), stand in for it. */
        struct statfs v;
        if (statfs(mounts[i].f_mntonname, &v) != 0) v = mounts[i];
        if (v.f_blocks == 0) continue;
        /* As df(1): used / (used + available to unprivileged users);
         * f_bavail goes negative inside the root reserve. */
        const double used  = (double)(v.f_blocks - v.f_bfree);
        const double avail = v.f_bavail > 0 ? (double)v.f_bavail : 0.0;
        const double pct   = used + avail > 0.0 ? used * 100.0 / (used + avail) : 0.0;
        const double ipct  = v.f_files > 0 && v.f_ffree >= 0
            ? (double)(v.f_files - (uint64_t)v.f_ffree) * 100.0 / (double)v.f_files : 0.0;
        ++s->fs.count;
        if (pct  > s->fs.max_used_percent)        s->fs.max_used_percent        = pct;
        if (ipct > s->fs.max_inodes_used_percent) s->fs.max_inodes_used_percent = ipct;

        if (d != NULL && d->nmounts < BUDYK_MAX_ENTITIES) {
            budyk_mount_c* m = &d->mounts[d->nmounts++];
            memset(m, 0, sizeof(*m));
            snprintf(m->mount,  sizeof(m->mount),  "%.255s", v.f_mntonname);
            snprintf(m->type,   sizeof(m->type),   "%.31s",  v.f_fstypename);
            snprintf(m->device, sizeof(m->device), "%.127s", v.f_mntfromname);
            const uint64_t bsize = (uint64_t)v.f_bsize;
            m->total_bytes         = (uint64_t)v.f_blocks * bsize;
            m->used_bytes          = (uint64_t)(v.f_blocks - v.f_bfree) * bsize;
            m->avail_bytes         = v.f_bavail > 0 ? (uint64_t)v.f_bavail * bsize : 0;
            m->used_percent        = pct;
            m->inodes_total        = (uint64_t)v.f_files;
            m->inodes_used         = v.f_ffree >= 0 && (uint64_t)v.f_ffree <= v.f_files
                                   ? v.f_files - (uint64_t)v.f_ffree : 0;
            m->inodes_used_percent = ipct;
        }
    }
    s->fs.present = 1;
    return 0;
}
