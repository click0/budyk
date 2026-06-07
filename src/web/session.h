// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace budyk {

// Process-local session table. Each token has a TTL; verify() refuses
// expired tokens and lazily evicts them. The store is thread-safe —
// HttpServer's worker calls into it concurrently with the collector.
//
// Tokens are 32-byte hex strings produced by web::auth::new_session_token().
//
// Optional disk persistence: when a persist path is set, the table is
// flushed (atomic temp+rename, mode 0600) after every mutation, so a
// daemon restart keeps logged-in admins logged in instead of bouncing
// every browser to the login screen. The file holds live bearer tokens
// — anyone who can read it can impersonate a session — hence 0600.
class SessionStore {
public:
    explicit SessionStore(uint64_t ttl_seconds = 86400);   // 24 h default

    // Mint a fresh token, persist it with the configured TTL, return it.
    // Empty string on entropy-source failure.
    std::string create();

    // True when `token` is present and not yet expired.
    bool        verify(const std::string& token);

    // Drop a token if present.
    void        revoke(const std::string& token);

    // Removes every entry whose deadline has passed; returns count
    // purged. Called opportunistically by verify() / create().
    size_t      purge_expired();

    size_t      size() const;

    // --- Persistence -----------------------------------------------------
    // Enable autosave to `path`: create()/revoke()/expiry-eviction will
    // rewrite the file. Pass "" to disable. Does not itself read or
    // write — call load() once at startup after setting this.
    void        set_persist_path(const std::string& path);

    // Load entries from `path`, skipping any already past their deadline.
    // A missing file is a no-op success (fresh install). Returns 0 on
    // success, negative on a malformed file. Merges into the current
    // table rather than replacing it.
    int         load(const char* path);

    // Atomically write the live table to `path` (temp + rename, 0600).
    // Returns 0 on success, -errno on failure. Normally invoked
    // automatically via the persist path; exposed for tests.
    int         save(const char* path) const;

private:
    // Snapshot the table under the lock and write it; used by the
    // autosave hook so file I/O never happens while mtx_ is held.
    void        autosave() const;

    mutable std::mutex mtx_;
    uint64_t           ttl_ns_;
    std::map<std::string, uint64_t> deadline_;   // token → expires_at_ns
    std::string        persist_path_;            // empty = RAM-only
};

} // namespace budyk
