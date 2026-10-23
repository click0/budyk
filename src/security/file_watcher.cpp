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

#include <algorithm>
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

// ---------------------------------------------------------------------------
// Path bookkeeping, the same on both kernels. A watch id (an inotify wd, a
// kqueue'd fd) is on an inode, not a path: after an editor's or visudo's
// write-new-then-rename, the path names another file and the watch is
// gone or on the old one. Such a path waits in pending_ and is watched
// again, reported as Created, as soon as it exists.
// ---------------------------------------------------------------------------

// One event per (path, kind) and poll() call: vim-style multi-write
// saves emit a flurry of IN_MODIFY on one rename.
struct FileWatcher::Batch {
    std::vector<FileChangeEvent>*         out;
    std::set<std::pair<std::string, int>> seen;
    int                                   appended = 0;

    void emit(const std::string& path, FileChangeKind kind) {
        if (!seen.insert(std::make_pair(path, static_cast<int>(kind))).second) return;
        out->push_back(FileChangeEvent{path, kind});
        ++appended;
    }
};

int FileWatcher::add(const std::string& path) {
    if (fd_ < 0) return -EBADF;
    auto it = path_to_id_.find(path);
    if (it != path_to_id_.end()) return it->second;
    const int id = sys_watch(path);
    if (id < 0) return id;
    bind(path, id);
    pending_.erase(path);
    return id;
}

void FileWatcher::remove(const std::string& path) {
    pending_.erase(path);
    unbind(path);
}

void FileWatcher::bind(const std::string& path, int id) {
    path_to_id_[path] = id;
    auto& paths = id_to_paths_[id];
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
}

void FileWatcher::unbind(const std::string& path) {
    auto it = path_to_id_.find(path);
    if (it == path_to_id_.end()) return;
    const int id = it->second;
    path_to_id_.erase(it);
    auto ids = id_to_paths_.find(id);
    if (ids == id_to_paths_.end()) return;
    auto& paths = ids->second;
    paths.erase(std::remove(paths.begin(), paths.end(), path), paths.end());
    if (paths.empty()) {
        // The last path on this inode: drop the watch (two paths to one
        // inode share an inotify wd).
        id_to_paths_.erase(ids);
        sys_unwatch(id);
    }
}

void FileWatcher::lost(const std::string& path) {
    unbind(path);
    pending_.insert(path);
}

void FileWatcher::retry_pending(Batch* b) {
    for (auto it = pending_.begin(); it != pending_.end(); ) {
        const int id = sys_watch(*it);
        if (id < 0) { ++it; continue; }
        bind(*it, id);
        b->emit(*it, FileChangeKind::Created);
        it = pending_.erase(it);
    }
}

void FileWatcher::shutdown() {
    if (fd_ < 0) return;
    for (const auto& kv : id_to_paths_) sys_unwatch(kv.first);
    ::close(fd_);
    fd_ = -1;
    id_to_paths_.clear();
    path_to_id_.clear();
    pending_.clear();
    overflowed_ = false;
}

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

int FileWatcher::sys_watch(const std::string& path) {
    // IN_MODIFY: content writes. IN_ATTRIB: mode, owner, link count.
    // IN_DELETE_SELF + IN_MOVE_SELF: plain rm, and the old file of a
    // rename-over save. Another path to the same inode gets the same wd.
    const uint32_t mask = IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;
    const int wd = ::inotify_add_watch(fd_, path.c_str(), mask);
    return wd < 0 ? -errno : wd;
}

void FileWatcher::sys_unwatch(int id) {
    // EINVAL when the kernel already dropped it (IN_IGNORED): harmless.
    ::inotify_rm_watch(fd_, id);
}

int FileWatcher::poll(int timeout_ms, std::vector<FileChangeEvent>* out) {
    if (fd_ < 0 || out == nullptr) return -EBADF;

    Batch batch{out, {}, 0};
    retry_pending(&batch);

    struct pollfd pfd{};
    pfd.fd     = fd_;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, batch.appended > 0 ? 0 : timeout_ms);
    if (pr < 0) return -errno;

    bool overflow = false;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (bool more = pr > 0; more; ) {
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -errno;
        }
        more = n > 0;

        char* p = buf;
        while (p < buf + n) {
            // The fixed part, copied out rather than cast: the name is not
            // read, and -Wcast-align=strict holds for the whole tree.
            struct inotify_event ev;
            std::memcpy(&ev, p, sizeof(ev));
            p += sizeof(struct inotify_event) + ev.len;
            if ((ev.mask & IN_Q_OVERFLOW) != 0) {
                overflow = true;
                continue;
            }
            auto it = id_to_paths_.find(ev.wd);
            if (it == id_to_paths_.end()) continue;   // a watch already let go
            const std::vector<std::string> paths = it->second;   // lost() edits the map
            // The inode is gone (IN_DELETE_SELF; IN_UNMOUNT; IN_IGNORED, the
            // kernel's last word on a watch) or no longer at the path
            // (IN_MOVE_SELF: the watch stays on it, wherever it went).
            const bool gone = (ev.mask & (IN_DELETE_SELF | IN_MOVE_SELF |
                                          IN_UNMOUNT | IN_IGNORED)) != 0;
            for (const auto& path : paths) {
                batch.emit(path, gone ? FileChangeKind::Deleted : FileChangeKind::Modified);
                if (gone) lost(path);
            }
        }
    }

    if (overflow) {
        // The kernel's queue filled and dropped events: which paths
        // changed is not known, so each is reported, and each watch is
        // checked against what its path names now.
        if (!overflowed_) {
            std::fprintf(stderr, "budyk: file watch: the event queue overflowed; "
                                 "reporting every watched path as changed\n");
            overflowed_ = true;
        }
        const auto live = path_to_id_;
        for (const auto& kv : live) {
            const int wd = sys_watch(kv.first);
            if (wd == kv.second) {
                batch.emit(kv.first, FileChangeKind::Modified);
                continue;
            }
            // Gone, or another file now: off the old inode, and watched
            // again below (a wd made just now for no other path is let go).
            batch.emit(kv.first, FileChangeKind::Deleted);
            lost(kv.first);
            if (wd >= 0 && id_to_paths_.count(wd) == 0) sys_unwatch(wd);
        }
    }

    // A path replaced by a rename is there again already: watch the new
    // file now, so the next change to it is not missed.
    retry_pending(&batch);
    return batch.appended;
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

int FileWatcher::sys_watch(const std::string& path) {
    const int file_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file_fd < 0) return -errno;
    struct kevent kev;
    EV_SET(&kev, file_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB
           | NOTE_EXTEND | NOTE_REVOKE,
           0, nullptr);
    if (::kevent(fd_, &kev, 1, nullptr, 0, nullptr) < 0) {
        const int e = -errno;
        ::close(file_fd);
        return e;
    }
    return file_fd;
}

void FileWatcher::sys_unwatch(int id) {
    // Closing the vnode's fd takes its kevent with it.
    ::close(id);
}

int FileWatcher::poll(int timeout_ms, std::vector<FileChangeEvent>* out) {
    if (fd_ < 0 || out == nullptr) return -EBADF;

    Batch batch{out, {}, 0};
    retry_pending(&batch);

    struct timespec  ts;
    struct timespec* tsp = nullptr;
    if (batch.appended > 0) timeout_ms = 0;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    constexpr int kMaxEvents = 32;
    struct kevent evs[kMaxEvents];
    const int     n = ::kevent(fd_, nullptr, 0, evs, kMaxEvents, tsp);
    if (n < 0) return -errno;

    for (int i = 0; i < n; ++i) {
        auto it = id_to_paths_.find(static_cast<int>(evs[i].ident));
        if (it == id_to_paths_.end()) continue;
        const std::vector<std::string> paths = it->second;   // lost() edits the map
        // Deleted, renamed away or revoked: the fd's vnode is not what
        // the path names any more.
        const bool gone = (evs[i].fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0;
        for (const auto& path : paths) {
            batch.emit(path, gone ? FileChangeKind::Deleted : FileChangeKind::Modified);
            if (gone) lost(path);
        }
    }

    // A path replaced by a rename is there again already: watch the new
    // file now, so the next change to it is not missed.
    retry_pending(&batch);
    return batch.appended;
}

#else
// ===========================================================================
// Other platforms — stub. Builds, returns -ENOSYS at runtime.
// ===========================================================================

int  FileWatcher::init()                                    { return -ENOSYS; }
int  FileWatcher::sys_watch(const std::string&)             { return -ENOSYS; }
void FileWatcher::sys_unwatch(int)                          {}
int  FileWatcher::poll  (int, std::vector<FileChangeEvent>*) { return -ENOSYS; }

#endif

} // namespace budyk
