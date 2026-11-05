// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace budyk {

/// Process-local session table. Each token has a TTL; verify() refuses
/// expired tokens and lazily evicts them. The store is thread-safe —
/// HttpServer's worker calls into it concurrently with the collector.
///
/// Tokens are 32-byte hex strings produced by web::auth::new_session_token().
///
/// Optional disk persistence: when a persist path is set, the table is
/// flushed (atomic temp+rename, mode 0600) after every mutation, so a
/// daemon restart keeps logged-in admins logged in instead of bouncing
/// every browser to the login screen. The file holds live bearer tokens
/// — anyone who can read it can impersonate a session — hence 0600.
class SessionStore {
public:
    explicit SessionStore(uint64_t ttl_seconds = 86400);   ///< 24 h default

    /// Mint a fresh token, persist it with the configured TTL, return it.
    /// Empty string on entropy-source failure.
    std::string create();

    /// True when `token` is present and not yet expired. A deadline past
    /// `now + TTL` (the clock stepped back since it was set) is brought
    /// down to that: every session grew by the step.
    bool        verify(const std::string& token);

    /// verify(), and a session in use is kept going (sliding expiration,
    /// spec §3.2): once an eighth of the TTL has passed since its deadline
    /// was set, the deadline is `now + TTL` again and the file rewritten.
    /// A request within that eighth is a verify() alone, so the file is
    /// not rewritten on every request. For the requests a session
    /// authenticates; the hub's check of an open WebSocket is verify().
    bool        touch(const std::string& token);

    /// Drop a token if present.
    void        revoke(const std::string& token);

    /// Removes every entry whose deadline has passed; returns count
    /// purged. Called opportunistically by verify() / create().
    size_t      purge_expired();

    size_t      size() const;

    // --- Persistence -----------------------------------------------------
    /// Enable autosave to `path`: create()/revoke()/expiry-eviction will
    /// rewrite the file. Pass "" to disable. Does not itself read or
    /// write — call load() once at startup after setting this. A save
    /// that fails is logged when its error changes (a full disk, a file
    /// left root's: nothing was saved, nothing said, and every browser
    /// signed in again after the restart).
    void        set_persist_path(const std::string& path);

    /// The credential the sessions are made under: a fingerprint of the
    /// password hash goes into the file, and load() drops a file made
    /// under another one. A password change ended no session: whoever
    /// had the old password kept the dashboard, for ever with the
    /// sliding expiration. Set before load(); "" (the default) writes
    /// and takes a file without one.
    void        set_credential(const std::string& password_hash);

    /// Load entries from `path`, skipping any already past their deadline
    /// and capping one past `now + TTL`. A missing file is a no-op
    /// success (fresh install); a file that cannot be read is -errno. A
    /// file made under another credential (see set_credential()) is
    /// left out whole, and `*stale` (may be null) says so when it held
    /// sessions. Merges into the current table rather than replacing it.
    int         load(const char* path, bool* stale = nullptr);

    /// Atomically write the live table to `path` (temp + rename, 0600,
    /// fsync'd: a power cut after a login left an empty file). Returns
    /// 0 on success, -errno on failure. Normally invoked automatically
    /// via the persist path; exposed for tests.
    int         save(const char* path) const;

private:
    /// Snapshot the table under the lock and write it; used by the
    /// autosave hook so file I/O never happens while mtx_ is held.
    void        autosave() const;

    mutable std::mutex mtx_;
    /// One save at a time, its snapshot and its file: they all write
    /// `<path>.tmp` and rename it. The HTTP thread (a login, a logout) and
    /// the main thread (a WebSocket's session expiring) saved at once, one
    /// truncated the other's file and a rename failed (ENOENT), or the
    /// older snapshot was the one left.
    mutable std::mutex save_mtx_;
    uint64_t           ttl_ns_;
    std::map<std::string, uint64_t> deadline_;   ///< token → expires_at_ns
    std::string        persist_path_;            ///< empty = RAM-only
    std::string        credential_;              ///< the hash's fingerprint, "" for none
    /// The last autosave's result, logged on change. Atomic: the HTTP
    /// thread (a login) and the main thread (an expired session evicted)
    /// autosave at once, and both wrote it without a lock.
    mutable std::atomic<int> last_save_rc_{0};
};

} // namespace budyk
