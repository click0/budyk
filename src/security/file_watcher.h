// SPDX-License-Identifier: BSD-3-Clause
//
/// @file
/// Cross-platform file change watcher — inotify on Linux, kqueue on
/// FreeBSD. Built for "alert me when /etc/sudoers changes" style rules:
/// the caller adds N file paths, then polls with a timeout and gets a
/// list of events back. No threading; the daemon's tick loop owns the
/// poll cadence.
///
/// Quirks worth knowing:
///
///   * A watch is on an inode, not a path (inotify's wd and kqueue's
///     EVFILT_VNODE alike). A save by rename (vim, visudo, sed -i,
///     a package manager) or an rm shows up as Deleted; the path then
///     waits, unwatched, and as soon as it exists again (often in the
///     same poll) it is watched anew and reported as Created. Before,
///     the first such save left the path unwatched for good.
///
///   * A path waiting to exist has the nearest directory above it that
///     exists watched for an entry made or moved in: its creation is an
///     event, which wakes a poll() (and the daemon's sleep on fd()).
///     It was looked up only when a poll() came, once a tick: up to the
///     L1 interval late, and one created and removed in between was
///     never seen. One that is there but cannot be watched (EACCES: an
///     ACL on the old file, which a rename-over save drops) is logged
///     once, and looked up on every poll().
///
///   * A directory reports its own changes and an entry made, removed
///     or moved in or out, on both kernels: a file written or chmod'ed
///     inside it is not a change of the directory (inotify reported one,
///     kqueue does not see one).
///
///   * On FreeBSD a watched path is held open (kqueue watches an open
///     file): its filesystem cannot be unmounted while budyk runs. A
///     FIFO, a socket or a device is refused (an open() of a FIFO waits
///     for a writer, and held the daemon's tick).
///
///   * Two paths to one inode (hard links) share an inotify wd; each
///     path gets the events.
///
///   * If inotify's queue overflows, every watched path is reported as
///     Modified and each watch is checked against what its path names
///     now (logged once).
///
///   * Linux IN_MODIFY can fire many times per logical write
///     (write(2)-per-line editors). We coalesce: at most one
///     Modified event per (path, poll-call).

#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace budyk {

enum class FileChangeKind {
    Modified = 0,
    Deleted  = 1,
    Created  = 2,
};

struct FileChangeEvent;   ///< forward — defined below

/// Aggregate state the daemon hands to the rule engine each tick.
/// `modifies` / `deletes` are cumulative-since-start counters per path;
/// `tampered_this_tick` is the set of paths that fired any event during
/// the most recent poll cycle (so rules can `when = files[p].tampered`).
struct FileWatchState {
    std::unordered_map<std::string, uint64_t> modifies;
    std::unordered_map<std::string, uint64_t> deletes;
    std::unordered_set<std::string>           tampered_this_tick;
    /// The paths configured: each has a `files` entry from the start (0,
    /// 0, false), where one appeared at its first event and a rule
    /// reading `files[p].tampered` failed until then.
    std::vector<std::string>                  watched;

    /// Apply a batch of events from FileWatcher::poll() — clears the
    /// tick-scoped flag set first, then bumps counters and records the
    /// tampered paths. Idempotent: an empty `events` only clears.
    void apply(const std::vector<FileChangeEvent>& events);
};

struct FileChangeEvent {
    std::string    path;
    FileChangeKind kind;
};

class FileWatcher {
public:
    FileWatcher();
    ~FileWatcher();

    /// Allocate the kernel handle (inotify_init1 / kqueue). Returns 0
    /// on success, -errno on failure.
    int  init();

    /// Close the kernel handle and forget every watched path.
    void shutdown();

    /// Begin watching `path`. Returns a watch id (>0) on success,
    /// -errno on failure. The id is opaque to the caller. A path that does
    /// not exist yet is 0: it is watched, and reported as Created, as soon
    /// as it exists (its directory's event wakes a poll()). A directory
    /// reports a change to its entries too (one made, removed or moved in
    /// or out).
    int  add(const std::string& path);

    /// Stop watching `path`. No-op if unknown.
    void remove(const std::string& path);

    /// Number of paths watched right now: a path deleted or replaced is
    /// not counted until it is watched again.
    std::size_t count() const;
    /// Number of paths waiting to exist (added while not there, or gone
    /// since): each is watched, and reported as Created, once it is.
    std::size_t pending() const;

    /// Block up to `timeout_ms` for at least one event, drain pending
    /// events, and append them to `out`. Returns the number appended,
    /// 0 on timeout, -errno on error. A negative `timeout_ms` blocks
    /// forever; 0 returns immediately (poll-style).
    int  poll(int timeout_ms, std::vector<FileChangeEvent>* out);

    /// The kernel handle (inotify's or kqueue's descriptor), readable
    /// when an event waits: for a sleep that wakes on a change. -1
    /// before init() and after shutdown().
    int  fd() const { return fd_; }

private:
    struct Batch;   ///< the events of one poll(), one per (path, kind)

    /// A path waiting to exist (added while not there, or gone since).
    struct Pending {
        std::string dir;              ///< the directory watched for it, "" none
        int         err     = 0;      ///< the error last logged for it (-errno), 0 none
        bool        recheck = true;   ///< looked up on the next poll whatever came
    };
    /// A directory watched for the pending paths under it.
    struct DirWatch {
        int id   = -1;   ///< its watch
        int refs = 0;    ///< how many pending paths it is watched for
    };

    /// The kernel side: watch `path` (an id >= 0, or -errno), let an id go.
    int  sys_watch(const std::string& path);
    /// Watch directory `dir` for an entry made or moved in, and for its
    /// own removal: an id >= 0, or -errno.
    int  sys_watch_dir(const std::string& dir);
    void sys_unwatch(int id);
    /// Whether a path's or a directory's watch uses `id` (inotify gives
    /// one wd per inode: a watched directory can be both).
    bool id_in_use(int id) const;
    /// path <-> id, and the id let go with its last path.
    void bind(const std::string& path, int id);
    void unbind(const std::string& path);
    /// The path's watch is gone or on another file: unbind, then pending.
    void lost(const std::string& path);
    /// Watch the nearest directory above `path` that exists, for `p`
    /// (the one it had let go, if another).
    void watch_parent(const std::string& path, Pending* p);
    /// `p` lets its directory go; the directory's last path, the watch.
    void release_parent(Pending* p);
    /// The directory watched as `id` is gone or moved: its paths look
    /// for another on the next lookup.
    void dir_gone(int id);
    /// Watch the pending paths that exist again; each is Created. With
    /// `all` (a watched directory changed, the queue overflowed) every
    /// one is looked up, else those not waiting on a directory's event.
    void retry_pending(Batch* b, bool all);

    int                                                fd_ = -1;
    std::unordered_map<int, std::vector<std::string>>  id_to_paths_;
    std::unordered_map<std::string, int>               path_to_id_;
    std::map<std::string, Pending>                     pending_;
    std::map<std::string, DirWatch>                    dirs_;
    std::unordered_map<int, std::string>               dir_of_id_;
    bool                                               overflowed_ = false;
};

} // namespace budyk
