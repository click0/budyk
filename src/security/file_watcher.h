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
    /// -errno on failure. The id is opaque to the caller.
    int  add(const std::string& path);

    /// Stop watching `path`. No-op if unknown.
    void remove(const std::string& path);

    /// Number of paths watched right now: a path deleted or replaced is
    /// not counted until it is watched again.
    std::size_t count() const;

    /// Block up to `timeout_ms` for at least one event, drain pending
    /// events, and append them to `out`. Returns the number appended,
    /// 0 on timeout, -errno on error. A negative `timeout_ms` blocks
    /// forever; 0 returns immediately (poll-style).
    int  poll(int timeout_ms, std::vector<FileChangeEvent>* out);

private:
    struct Batch;   ///< the events of one poll(), one per (path, kind)

    /// The kernel side: watch `path` (an id >= 0, or -errno), let an id go.
    int  sys_watch(const std::string& path);
    void sys_unwatch(int id);
    /// path <-> id, and the id let go with its last path.
    void bind(const std::string& path, int id);
    void unbind(const std::string& path);
    /// The path's watch is gone or on another file: unbind, then pending.
    void lost(const std::string& path);
    /// Watch every pending path that exists again; each is Created.
    void retry_pending(Batch* b);

    int                                                fd_ = -1;
    std::unordered_map<int, std::vector<std::string>>  id_to_paths_;
    std::unordered_map<std::string, int>               path_to_id_;
    std::set<std::string>                              pending_;
    bool                                               overflowed_ = false;
};

} // namespace budyk
