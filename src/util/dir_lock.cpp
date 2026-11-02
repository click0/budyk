// SPDX-License-Identifier: BSD-3-Clause
#include "util/dir_lock.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace budyk {

namespace {

std::string lock_path(const char* dir) {
    return std::string(dir) + "/budyk.lock";
}

// The holder's pid as it wrote it, without the newline.
std::string read_holder(int fd) {
    char buf[32] = {};
    const ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
    std::string s = n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

} // namespace

int lock_data_dir(const char* dir, std::string* holder) {
    if (dir == nullptr) return -EINVAL;
    const std::string path = lock_path(dir);
    // The lock is a plain file with one name, or nothing is done with
    // it: the data directory's owner (the service user) decides what
    // `budyk.lock` is, and root (`sudo budyk fsck --repair`, a `sudo
    // budyk serve`) followed it, gave the file it led to away to that
    // owner, truncated it and wrote a pid into it. A symlink or a hard
    // link to /etc/shadow was a local root. O_NOFOLLOW refuses the
    // symlink (ELOOP; EMLINK on FreeBSD), fstat() the rest.
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        const int err = errno;
        if (err == ELOOP || err == EMLINK) {
            std::fprintf(stderr,
                "budyk: '%s' is a symbolic link; the lock must be a plain file: remove it\n",
                path.c_str());
            return -ELOOP;
        }
        return -err;
    }
    struct stat fst{}, dst{};
    if (::fstat(fd, &fst) != 0) {
        const int err = errno;
        ::close(fd);
        return -err;
    }
    if (!S_ISREG(fst.st_mode) || fst.st_nlink != 1) {
        std::fprintf(stderr,
            "budyk: '%s' is %s; the lock must be a plain file with one name: remove it\n",
            path.c_str(), S_ISREG(fst.st_mode) ? "a hard link" : "not a regular file");
        ::close(fd);
        return S_ISREG(fst.st_mode) ? -ELOOP : -EINVAL;
    }
    // Root (`sudo budyk fsck --repair`) leaves the lock file the data
    // directory's owner's: made root's, 0600, it kept the service, which
    // runs as that owner (budyk), from starting ("cannot lock: Permission
    // denied"). One a run of that left behind is given back the same
    // way. Only a file that is root's: the owner's own is left as it is.
    if (::geteuid() == 0 && fst.st_uid == 0 && ::stat(dir, &dst) == 0 &&
        (fst.st_uid != dst.st_uid || fst.st_gid != dst.st_gid)) {
        if (::fchown(fd, dst.st_uid, dst.st_gid) != 0) {
            std::fprintf(stderr, "budyk: cannot give '%s' to the owner of '%s': %s\n",
                         path.c_str(), dir, std::strerror(errno));
        }
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        if (holder != nullptr) *holder = read_holder(fd);
        ::close(fd);
        return err == EWOULDBLOCK ? -EWOULDBLOCK : -err;
    }
    char pid[32];
    const int n = std::snprintf(pid, sizeof(pid), "%ld\n", static_cast<long>(::getpid()));
    if (::ftruncate(fd, 0) != 0 ||
        ::pwrite(fd, pid, static_cast<size_t>(n), 0) != static_cast<ssize_t>(n)) {
        const int err = errno;
        ::close(fd);
        return -err;
    }
    return fd;
}

bool data_dir_locked(const char* dir, std::string* holder) {
    if (dir == nullptr) return false;
    const std::string path = lock_path(dir);
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    bool locked = false;
    if (::flock(fd, LOCK_SH | LOCK_NB) != 0) {
        locked = errno == EWOULDBLOCK;
        if (locked && holder != nullptr) *holder = read_holder(fd);
    } else {
        ::flock(fd, LOCK_UN);
    }
    ::close(fd);
    return locked;
}

} // namespace budyk
