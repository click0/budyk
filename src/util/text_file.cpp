// SPDX-License-Identifier: BSD-3-Clause
#include "util/text_file.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace budyk {

int read_text_file(const char* path, std::string* text, std::string* error) {
    std::string unused;
    if (error == nullptr) error = &unused;
    error->clear();
    if (path == nullptr || text == nullptr) {
        *error = "no path";
        return -EINVAL;
    }
    auto failed = [error](int err, const char* what) {
        *error = what != nullptr ? what : std::strerror(err);
        return -err;
    };
    // O_NONBLOCK: open() of a FIFO with no writer would wait for one.
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return failed(errno, nullptr);
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        return failed(err, nullptr);
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        return S_ISDIR(st.st_mode) ? failed(EISDIR, nullptr)
                                   : failed(ENOTSUP, "not a regular file");
    }
    if (st.st_size > static_cast<off_t>(kTextFileMax)) {
        ::close(fd);
        return failed(EFBIG, "larger than 16 MiB");
    }
    // A regular file's read() never waits: the flag can stay. The size
    // is a hint (the file may grow as it is read): read to the end,
    // within the bound.
    text->clear();
    text->reserve(static_cast<size_t>(st.st_size));
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            ::close(fd);
            return failed(err, nullptr);
        }
        if (n == 0) break;
        if (text->size() + static_cast<size_t>(n) > kTextFileMax) {
            ::close(fd);
            return failed(EFBIG, "larger than 16 MiB");
        }
        text->append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return 0;
}

} // namespace budyk
