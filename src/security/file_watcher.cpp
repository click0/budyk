// SPDX-License-Identifier: BSD-3-Clause
#include "security/file_watcher.h"

#include <sys/types.h>
#include <unistd.h>

#if defined(BUDYK_LINUX)
#include <poll.h>
#include <sys/inotify.h>
#elif defined(BUDYK_FREEBSD)
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>

namespace budyk {

void FileWatchState::apply(const std::vector<FileChangeEvent>& events) {
    tampered_this_tick.clear();
    for (const auto& ev : events) {
        tampered_this_tick.insert(ev.path);
        if (ev.kind == FileChangeKind::Deleted) {
            ++deletes[ev.path];
        } else {
            // Modified + Created both count as modifies for rule
            // ergonomics — operators usually just care that "the file
            // changed somehow".
            ++modifies[ev.path];
        }
    }
}

FileWatcher::FileWatcher() = default;

FileWatcher::~FileWatcher() { shutdown(); }

std::size_t FileWatcher::count() const { return path_to_id_.size(); }

#if defined(BUDYK_LINUX)
// ===========================================================================
// Linux: inotify
// ===========================================================================

int FileWatcher::init() {
    if (fd_ >= 0) return -EBUSY;
    fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ < 0) return -errno;
    return 0;
}

void FileWatcher::shutdown() {
    if (fd_ < 0) return;
    for (const auto& kv : id_to_path_) {
        ::inotify_rm_watch(fd_, kv.first);
    }
    ::close(fd_);
    fd_ = -1;
    id_to_path_.clear();
    path_to_id_.clear();
}

int FileWatcher::add(const std::string& path) {
    if (fd_ < 0) return -EBADF;
    // IN_MODIFY: content writes. IN_DELETE_SELF + IN_MOVE_SELF: handle
    // editor "rename-over save" + plain rm. IN_CREATE: when watching a
    // directory parent (out of scope here but cheap to flag).
    const uint32_t mask = IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF
                        | IN_ATTRIB;
    const int wd = ::inotify_add_watch(fd_, path.c_str(), mask);
    if (wd < 0) return -errno;
    id_to_path_[wd]    = path;
    path_to_id_[path]  = wd;
    return wd;
}

void FileWatcher::remove(const std::string& path) {
    auto it = path_to_id_.find(path);
    if (it == path_to_id_.end()) return;
    ::inotify_rm_watch(fd_, it->second);
    id_to_path_.erase(it->second);
    path_to_id_.erase(it);
}

int FileWatcher::poll(int timeout_ms, std::vector<FileChangeEvent>* out) {
    if (fd_ < 0 || out == nullptr) return -EBADF;

    struct pollfd pfd{};
    pfd.fd     = fd_;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr <  0) return -errno;
    if (pr == 0) return 0;

    // Coalesce per (path, kind) inside this drain — vim-style
    // multi-write saves emit a flurry of IN_MODIFY on one rename.
    std::set<std::pair<std::string, int>> seen;
    int                                   appended = 0;

    char    buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -errno;
        }
        if (n == 0) break;

        char* p = buf;
        while (p < buf + n) {
            auto* ev = reinterpret_cast<struct inotify_event*>(p);
            auto  it = id_to_path_.find(ev->wd);
            if (it != id_to_path_.end()) {
                FileChangeEvent out_ev;
                out_ev.path = it->second;
                if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                    out_ev.kind = FileChangeKind::Deleted;
                } else if (ev->mask & IN_CREATE) {
                    out_ev.kind = FileChangeKind::Created;
                } else {
                    out_ev.kind = FileChangeKind::Modified;
                }
                const auto key = std::make_pair(
                    out_ev.path, static_cast<int>(out_ev.kind));
                if (seen.insert(key).second) {
                    out->push_back(std::move(out_ev));
                    ++appended;
                }
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return appended;
}

#elif defined(BUDYK_FREEBSD)
// ===========================================================================
// FreeBSD: kqueue + EVFILT_VNODE
// ===========================================================================

int FileWatcher::init() {
    if (fd_ >= 0) return -EBUSY;
    fd_ = ::kqueue();
    if (fd_ < 0) return -errno;
    return 0;
}

void FileWatcher::shutdown() {
    if (fd_ < 0) return;
    // Each watch id is an open fd we hold for EVFILT_VNODE.
    for (const auto& kv : id_to_path_) {
        ::close(kv.first);
    }
    ::close(fd_);
    fd_ = -1;
    id_to_path_.clear();
    path_to_id_.clear();
}

int FileWatcher::add(const std::string& path) {
    if (fd_ < 0) return -EBADF;
    const int file_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file_fd < 0) return -errno;
    struct kevent kev;
    EV_SET(&kev, file_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB
           | NOTE_EXTEND,
           0, nullptr);
    if (::kevent(fd_, &kev, 1, nullptr, 0, nullptr) < 0) {
        const int e = -errno;
        ::close(file_fd);
        return e;
    }
    id_to_path_[file_fd] = path;
    path_to_id_[path]    = file_fd;
    return file_fd;
}

void FileWatcher::remove(const std::string& path) {
    auto it = path_to_id_.find(path);
    if (it == path_to_id_.end()) return;
    struct kevent kev;
    EV_SET(&kev, it->second, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
    ::kevent(fd_, &kev, 1, nullptr, 0, nullptr);
    ::close(it->second);
    id_to_path_.erase(it->second);
    path_to_id_.erase(it);
}

int FileWatcher::poll(int timeout_ms, std::vector<FileChangeEvent>* out) {
    if (fd_ < 0 || out == nullptr) return -EBADF;
    struct timespec  ts;
    struct timespec* tsp = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    constexpr int kMaxEvents = 32;
    struct kevent evs[kMaxEvents];
    const int     n = ::kevent(fd_, nullptr, 0, evs, kMaxEvents, tsp);
    if (n <  0) return -errno;
    if (n == 0) return 0;

    std::set<std::pair<std::string, int>> seen;
    int                                   appended = 0;
    for (int i = 0; i < n; ++i) {
        auto it = id_to_path_.find(static_cast<int>(evs[i].ident));
        if (it == id_to_path_.end()) continue;
        FileChangeEvent ev;
        ev.path = it->second;
        if (evs[i].fflags & (NOTE_DELETE | NOTE_RENAME)) {
            ev.kind = FileChangeKind::Deleted;
        } else {
            ev.kind = FileChangeKind::Modified;
        }
        const auto key = std::make_pair(ev.path, static_cast<int>(ev.kind));
        if (seen.insert(key).second) {
            out->push_back(std::move(ev));
            ++appended;
        }
    }
    return appended;
}

#else
// ===========================================================================
// Other platforms — stub. Builds, returns -ENOSYS at runtime.
// ===========================================================================

int  FileWatcher::init()                                    { return -ENOSYS; }
void FileWatcher::shutdown()                                {}
int  FileWatcher::add   (const std::string&)                { return -ENOSYS; }
void FileWatcher::remove(const std::string&)                {}
int  FileWatcher::poll  (int, std::vector<FileChangeEvent>*) { return -ENOSYS; }

#endif

} // namespace budyk
