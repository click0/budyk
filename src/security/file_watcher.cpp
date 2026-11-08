// SPDX-License-Identifier: BSD-3-Clause
#include "security/file_watcher.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(BUDYK_LINUX)
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
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
std::size_t FileWatcher::replaced_outside() const { return outside_.size(); }

std::size_t FileWatcher::pending() const { return pending_.size(); }

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
    if (id == -ENOENT) {
        // Not there yet (a sudoers.d drop-in, a log file, a filesystem
        // mounted later): watched, as Created, as soon as it exists. Its
        // directory is watched for it; the next poll looks it up once
        // too (it may have come between the two calls).
        Pending& p = pending_[path];
        watch_parent(path, &p);
        return 0;
    }
    if (id < 0) return id;
    bind(path, id);
    if (auto pd = pending_.find(path); pd != pending_.end()) {
        release_parent(&pd->second);
        pending_.erase(pd);
    }
    return id;
}

void FileWatcher::remove(const std::string& path) {
    if (auto pd = pending_.find(path); pd != pending_.end()) {
        release_parent(&pd->second);
        pending_.erase(pd);
    }
    unbind(path);
}

bool FileWatcher::id_in_use(int id) const {
    return id_to_paths_.count(id) != 0 || dir_of_id_.count(id) != 0;
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
        // inode share an inotify wd), unless a pending path's directory
        // watch is the same wd.
        id_to_paths_.erase(ids);
        if (!id_in_use(id)) sys_unwatch(id);
    }
}

void FileWatcher::lost(const std::string& path) {
    unbind(path);
    pending_[path].recheck = true;
}

void FileWatcher::watch_parent(const std::string& path, Pending* p) {
    // The nearest directory above the path that is there: a path of
    // /etc/sudoers.d/x with no sudoers.d waits on /etc.
    std::string d = path;
    for (;;) {
        const size_t slash = d.find_last_of('/');
        if (slash == std::string::npos) {
            d = ".";
        } else {
            d.resize(slash == 0 ? 1 : slash);
        }
        struct stat st{};
        if (::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) break;
        if (d == "/" || d == ".") {   // nothing above it to watch
            release_parent(p);
            return;
        }
    }
    if (d == p->dir) return;
    release_parent(p);
    auto it = dirs_.find(d);
    if (it == dirs_.end()) {
        const int id = sys_watch_dir(d);
        if (id < 0) return;   // looked up on every poll instead
        // inotify: one wd per inode, so another name of a directory
        // watched already gives its wd.
        if (const auto known = dir_of_id_.find(id); known != dir_of_id_.end()) {
            d  = known->second;
            it = dirs_.find(d);
        } else {
            it = dirs_.emplace(d, DirWatch{id, 0}).first;
            dir_of_id_[id] = d;
        }
    }
    ++it->second.refs;
    p->dir = d;
}

void FileWatcher::release_parent(Pending* p) {
    if (p->dir.empty()) return;
    auto it = dirs_.find(p->dir);
    p->dir.clear();
    if (it == dirs_.end() || --it->second.refs > 0) return;
    const int id = it->second.id;
    dirs_.erase(it);
    dir_of_id_.erase(id);
    if (!id_in_use(id)) sys_unwatch(id);
}

void FileWatcher::dir_gone(int id) {
    const auto known = dir_of_id_.find(id);
    if (known == dir_of_id_.end()) return;
    const std::string name = known->second;
    for (auto& kv : pending_) {
        if (kv.second.dir == name) {
            kv.second.dir.clear();
            kv.second.recheck = true;
        }
    }
    dirs_.erase(name);
    dir_of_id_.erase(known);
    if (!id_in_use(id)) sys_unwatch(id);
}

void FileWatcher::retry_pending(Batch* b, bool all) {
    for (auto it = pending_.begin(); it != pending_.end(); ) {
        Pending& p = it->second;
        // One waiting on its directory's event is left until one comes;
        // one with no directory watched, or there but not watchable, is
        // looked up on every poll.
        if (!all && !p.recheck && !p.dir.empty() && p.err == 0) {
            ++it;
            continue;
        }
        p.recheck = false;
        const int id = sys_watch(it->first);
        if (id >= 0) {
            if (p.err != 0) {
                std::fprintf(stderr, "budyk: file watch: '%s' is watched again\n",
                             it->first.c_str());
            }
            release_parent(&p);
            bind(it->first, id);
            b->emit(it->first, FileChangeKind::Created);
            it = pending_.erase(it);
            continue;
        }
        if (id == -ENOENT || id == -ENOTDIR) {
            p.err = 0;
        } else if (id != p.err) {
            // There, but not to be watched: a rename-over save (visudo,
            // vipw) dropped the ACL that let budyk read the old file, or
            // inotify's max_user_watches is spent. It was silent, and
            // every later change unseen.
            std::fprintf(stderr,
                "budyk: file watch: '%s' is there but cannot be watched: %s; "
                "tried again on every tick\n", it->first.c_str(), std::strerror(-id));
            p.err = id;
        }
        watch_parent(it->first, &p);
        ++it;
    }
}

void FileWatcher::shutdown() {
    if (fd_ < 0) return;
    std::set<int> ids;
    for (const auto& kv : id_to_paths_) ids.insert(kv.first);
    for (const auto& kv : dir_of_id_) ids.insert(kv.first);
    for (const int id : ids) sys_unwatch(id);
    ::close(fd_);
    fd_ = -1;
    id_to_paths_.clear();
    path_to_id_.clear();
    pending_.clear();
    dirs_.clear();
    dir_of_id_.clear();
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
    // A directory also reports an entry made, removed or moved in or out
    // (`install x /etc/cron.d/`, `rm /etc/sudoers.d/x`), as kqueue's
    // NOTE_WRITE on a directory does on FreeBSD; a write inside it was
    // all inotify gave.
    uint32_t mask = IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        mask |= IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
    }
    const int wd = ::inotify_add_watch(fd_, path.c_str(), mask);
    return wd < 0 ? -errno : wd;
}

int FileWatcher::sys_watch_dir(const std::string& dir) {
    // IN_MASK_ADD: the directory may be a watched path too, the same wd,
    // and its own mask stays as it was.
    const int wd = ::inotify_add_watch(fd_, dir.c_str(),
        IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_MASK_ADD);
    return wd < 0 ? -errno : wd;
}

void FileWatcher::sys_unwatch(int id) {
    // EINVAL when the kernel already dropped it (IN_IGNORED): harmless.
    ::inotify_rm_watch(fd_, id);
}

int FileWatcher::poll(int timeout_ms, std::vector<FileChangeEvent>* out) {
    if (fd_ < 0 || out == nullptr) return -EBADF;

    Batch batch{out, {}, 0};
    retry_pending(&batch, false);
    bool dir_changed = false;   // a pending path's directory saw an entry come

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
            if (dir_of_id_.count(ev.wd) != 0) {
                dir_changed = true;
                if ((ev.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED)) != 0) {
                    dir_gone(ev.wd);
                }
            }
            auto it = id_to_paths_.find(ev.wd);
            if (it == id_to_paths_.end()) continue;   // a watch already let go
            // A watched directory's own events, and an entry made, removed
            // or moved in it: a file written or chmod'ed inside (an event
            // with a name, not of those four) is not a change of the
            // directory. inotify reported it as one, kqueue does not see
            // it: a rules file shared by both platforms read two things.
            if (ev.len > 0 &&
                (ev.mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) == 0) {
                continue;
            }
            const std::vector<std::string> paths = it->second;   // lost() edits the map
            // The inode is gone (IN_DELETE_SELF; IN_UNMOUNT; IN_IGNORED, the
            // kernel's last word on a watch) or no longer at the path
            // (IN_MOVE_SELF: the watch stays on it, wherever it went).
            const bool gone = (ev.mask & (IN_DELETE_SELF | IN_MOVE_SELF |
                                          IN_UNMOUNT | IN_IGNORED)) != 0;
            for (const auto& path : paths) {
                // IN_ATTRIB (the link count) is all the old file of a
                // rename-over save gets while it lives on, held open by a
                // reader or with another hard link: IN_DELETE_SELF comes
                // when the last of those goes, or never, and until then
                // the new file at the path was not watched (vipw, sed -i,
                // a package upgrade, while anyone read /etc/passwd). The
                // path is asked again: another file there, or none, is
                // the old one gone, and the new one is watched below.
                bool off = gone;
                if (!off && (ev.mask & IN_ATTRIB) != 0) {
                    const int wd = sys_watch(path);
                    off = wd != ev.wd;
                    if (off && wd >= 0 && id_to_paths_.count(wd) == 0) sys_unwatch(wd);
                    // Still the old file at the path, and it has no name
                    // left: replaced where this mount namespace does not
                    // see, under a file bind-mounted on its own (docker
                    // -v FILE:FILE). Nothing that comes later reaches it:
                    // the watch went blind without a word.
                    struct stat pst{};
                    if (!off && ::stat(path.c_str(), &pst) == 0 && pst.st_nlink == 0 &&
                        outside_.insert(path).second) {
                        std::fprintf(stderr, "budyk: file watch: %s was replaced outside this "
                                     "mount namespace and still names the old file (a file "
                                     "bind-mounted on its own keeps it): later changes are not "
                                     "seen; mount its directory instead\n", path.c_str());
                    }
                }
                batch.emit(path, off ? FileChangeKind::Deleted : FileChangeKind::Modified);
                if (off) lost(path);
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
    // file now, so the next change to it is not missed. A directory's
    // event (or the overflow) has every pending path looked up.
    retry_pending(&batch, dir_changed || overflow);
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

namespace {

// A regular file or a directory, the only kinds watched: an open() of a
// FIFO for reading waits for a writer (a watched path replaced by a FIFO
// held the daemon's tick, with every collection and alert), and one of
// a device can do what the device does on open.
bool watchable(const struct stat& st) {
    return S_ISREG(st.st_mode) || S_ISDIR(st.st_mode);
}

// `fd` open on `path` for an EVFILT_VNODE of `fflags` in kqueue `kq`: the
// fd as the id, or -errno (the fd closed).
int vnode_watch(int kq, int fd, unsigned int fflags) {
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, fflags, 0, nullptr);
    if (::kevent(kq, &kev, 1, nullptr, 0, nullptr) < 0) {
        const int e = -errno;
        ::close(fd);
        return e;
    }
    return fd;
}

} // namespace

int FileWatcher::sys_watch(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return -errno;
    if (!watchable(st)) return -EFTYPE;
    // O_NONBLOCK all the same: the path may name a FIFO by the open().
    const int file_fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (file_fd < 0) return -errno;
    if (::fstat(file_fd, &st) != 0 || !watchable(st)) {
        ::close(file_fd);
        return -EFTYPE;
    }
    // NOTE_LINK: a hard link made or removed, as inotify's IN_ATTRIB.
    return vnode_watch(fd_, file_fd,
                       NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB |
                       NOTE_EXTEND | NOTE_LINK | NOTE_REVOKE);
}

int FileWatcher::sys_watch_dir(const std::string& dir) {
    const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC);
    if (dir_fd < 0) return -errno;
    // NOTE_WRITE on a directory: an entry made, removed or renamed.
    return vnode_watch(fd_, dir_fd, NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE);
}

void FileWatcher::sys_unwatch(int id) {
    // Closing the vnode's fd takes its kevent with it.
    ::close(id);
}

int FileWatcher::poll(int timeout_ms, std::vector<FileChangeEvent>* out) {
    if (fd_ < 0 || out == nullptr) return -EBADF;

    Batch batch{out, {}, 0};
    retry_pending(&batch, false);
    bool dir_changed = false;   // a pending path's directory saw an entry come

    struct timespec  ts;
    struct timespec* tsp = nullptr;
    if (batch.appended > 0) timeout_ms = 0;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    // Every event queued, not the first 32: a `pkg upgrade` that changed
    // a hundred watched files was reported over four ticks.
    constexpr int kMaxEvents = 32;
    struct kevent evs[kMaxEvents];
    for (int n = kMaxEvents; n == kMaxEvents; ) {
        n = ::kevent(fd_, nullptr, 0, evs, kMaxEvents, tsp);
        if (n < 0) return -errno;
        ts  = timespec{};   // the rest, if any, without waiting
        tsp = &ts;

        for (int i = 0; i < n; ++i) {
            const int id = static_cast<int>(evs[i].ident);
            if (dir_of_id_.count(id) != 0) {
                dir_changed = true;
                if ((evs[i].fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0) dir_gone(id);
                continue;
            }
            auto it = id_to_paths_.find(id);
            if (it == id_to_paths_.end()) continue;
            const std::vector<std::string> paths = it->second;   // lost() edits the map
            // Deleted, renamed away or revoked: the fd's vnode is not what
            // the path names any more. NOTE_DELETE comes for any name of
            // the file removed, NOTE_RENAME for any renamed: a path that
            // still names it (another hard link went) is a change.
            const bool gone = (evs[i].fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)) != 0;
            struct stat fst{};
            const bool open_ok = (evs[i].fflags & NOTE_REVOKE) == 0 &&
                                 ::fstat(id, &fst) == 0 && fst.st_nlink > 0;
            for (const auto& path : paths) {
                bool off = gone;
                if (off && open_ok) {
                    struct stat pst{};
                    off = ::stat(path.c_str(), &pst) != 0 ||
                          pst.st_dev != fst.st_dev || pst.st_ino != fst.st_ino;
                }
                batch.emit(path, off ? FileChangeKind::Deleted : FileChangeKind::Modified);
                if (off) lost(path);
            }
        }
    }

    // A path replaced by a rename is there again already: watch the new
    // file now, so the next change to it is not missed. A directory's
    // event has every pending path looked up.
    retry_pending(&batch, dir_changed);
    return batch.appended;
}

#else
// ===========================================================================
// Other platforms — stub. Builds, returns -ENOSYS at runtime.
// ===========================================================================

int  FileWatcher::init()                                    { return -ENOSYS; }
int  FileWatcher::sys_watch(const std::string&)             { return -ENOSYS; }
int  FileWatcher::sys_watch_dir(const std::string&)         { return -ENOSYS; }
void FileWatcher::sys_unwatch(int)                          {}
int  FileWatcher::poll  (int, std::vector<FileChangeEvent>*) { return -ENOSYS; }

#endif

} // namespace budyk
