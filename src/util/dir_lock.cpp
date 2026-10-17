// SPDX-License-Identifier: BSD-3-Clause
#include "util/dir_lock.h"

#include <cerrno>
#include <cstdio>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
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
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
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
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
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
