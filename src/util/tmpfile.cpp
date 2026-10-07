// SPDX-License-Identifier: BSD-3-Clause
#include "util/tmpfile.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

namespace budyk {

bool write_private_tmp(const char* prefix, const std::string& body,
                       char* path_out, size_t cap) {
    if (prefix == nullptr || path_out == nullptr) return false;
    const char* dir = std::getenv("TMPDIR");
    if (dir == nullptr || *dir == '\0') dir = "/tmp";
    const int n = std::snprintf(path_out, cap, "%s/%sXXXXXX", dir, prefix);
    if (n < 0 || static_cast<size_t>(n) >= cap) {
        errno = ENAMETOOLONG;
        return false;
    }

    const int fd = ::mkstemp(path_out);
    if (fd < 0) return false;

    size_t done = 0;
    while (done < body.size()) {
        const ssize_t w = ::write(fd, body.data() + done, body.size() - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            ::close(fd);
            ::unlink(path_out);
            errno = err;
            return false;
        }
        done += static_cast<size_t>(w);
    }
    ::close(fd);
    return true;
}

} // namespace budyk
