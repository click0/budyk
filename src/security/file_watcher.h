// SPDX-License-Identifier: BSD-3-Clause
//
// Cross-platform file change watcher — inotify on Linux, kqueue on
// FreeBSD. Built for "alert me when /etc/sudoers changes" style rules:
// the caller adds N file paths, then polls with a timeout and gets a
// list of events back. No threading; the daemon's tick loop owns the
// poll cadence.
//
// Quirks worth knowing:
//
//   * On both platforms, "editor save by rename" (vim's default,
//     :w in JOE) shows up as Deleted on the original path. The
//     caller can re-add the path after a Deleted event to keep
//     tracking — we don't auto-renew on FreeBSD because kqueue's
//     EVFILT_VNODE is bound to a specific inode, not a path.
//
//   * Linux IN_MODIFY can fire many times per logical write
//     (write(2)-per-line editors). We coalesce: at most one
//     Modified event per (path, poll-call).

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace budyk {

enum class FileChangeKind {
    Modified = 0,
    Deleted  = 1,
    Created  = 2,
};

struct FileChangeEvent;   // forward — defined below

// Aggregate state the daemon hands to the rule engine each tick.
// `modifies` / `deletes` are cumulative-since-start counters per path;
// `tampered_this_tick` is the set of paths that fired any event during
// the most recent poll cycle (so rules can `when = files[p].tampered`).
struct FileWatchState {
    std::unordered_map<std::string, uint64_t> modifies;
    std::unordered_map<std::string, uint64_t> deletes;
    std::unordered_set<std::string>           tampered_this_tick;

    // Apply a batch of events from FileWatcher::poll() — clears the
    // tick-scoped flag set first, then bumps counters and records the
    // tampered paths. Idempotent: an empty `events` only clears.
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

    // Allocate the kernel handle (inotify_init1 / kqueue). Returns 0
    // on success, -errno on failure.
    int  init();

    // Close the kernel handle and forget every watched path.
    void shutdown();

    // Begin watching `path`. Returns a watch id (>0) on success,
    // -errno on failure. The id is opaque to the caller.
    int  add(const std::string& path);

    // Stop watching `path`. No-op if unknown.
    void remove(const std::string& path);

    // Number of currently watched paths. Useful for tests.
    std::size_t count() const;

    // Block up to `timeout_ms` for at least one event, drain pending
    // events, and append them to `out`. Returns the number appended,
    // 0 on timeout, -errno on error. A negative `timeout_ms` blocks
    // forever; 0 returns immediately (poll-style).
    int  poll(int timeout_ms, std::vector<FileChangeEvent>* out);

private:
    int                                       fd_ = -1;
    std::unordered_map<int, std::string>      id_to_path_;
    std::unordered_map<std::string, int>      path_to_id_;
};

} // namespace budyk
