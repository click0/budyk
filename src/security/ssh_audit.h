// SPDX-License-Identifier: BSD-3-Clause
//
// SSH auth-log scanner — counts failed login attempts, invalid-user
// hits and successful authentications. Used to feed brute-force
// detection rules. The scanner keeps a byte offset into the log file
// between calls and only counts new lines; log rotation (file shrunk
// below the cached offset) resets back to 0.
//
// Format expected is the standard sshd(8) syslog output:
//   "Failed password for [invalid user] <user> from <ip> port <p> ssh2"
//   "Invalid user <user> from <ip> port <p>"
//   "Accepted (password|publickey) for <user> from <ip> port <p> ssh2"
//
// Path defaults are platform-dependent and supplied by the caller:
//   /var/log/auth.log   — Debian/Ubuntu, FreeBSD
//   /var/log/secure     — RHEL/Fedora
//   journalctl(1)       — systemd journal (out of scope; consumers may
//                         pipe `journalctl -u sshd -o cat` into a tail
//                         file and point the scanner at that)

#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace budyk {

struct SshAuditStats {
    uint64_t failed_password = 0;     // "Failed password for ... from ..."
    uint64_t invalid_user    = 0;     // "Invalid user ... from ..."
    uint64_t accepted        = 0;     // "Accepted password|publickey for ..."
    // Top offenders / targets by hit count. Capped at 16 entries each
    // so an attacker can't blow memory by cycling a million bogus IPs.
    std::vector<std::pair<std::string, uint64_t>> top_ips;
    std::vector<std::pair<std::string, uint64_t>> top_users;
};

class SshAuditScanner {
public:
    SshAuditScanner();

    // Reset state — counters, offset, top-N tables. Useful in tests
    // and on operator-driven log rotation.
    void reset();

    // Scan `path` from the cached offset to EOF; updates counters and
    // top-N tables in-place. Returns 0 on success, -errno on open/read
    // failure. A path that doesn't exist is *not* a hard error — the
    // scanner just returns 0 with no new counts (a fresh install often
    // has no auth.log yet).
    int scan(const char* path, SshAuditStats* out);

    // For tests: feed a single log line directly, no I/O involved.
    void ingest_line(const std::string& line);

    // Aggregate stats since construction / last reset(). Top-N tables
    // are derived on read.
    SshAuditStats snapshot() const;

    // Current byte offset into the watched file. Exposed for tests
    // and observability.
    uint64_t offset() const;

private:
    void recompute_top(std::vector<std::pair<std::string, uint64_t>>& out,
                       const std::unordered_map<std::string, uint64_t>& src) const;

    uint64_t                                   offset_ = 0;
    uint64_t                                   failed_password_ = 0;
    uint64_t                                   invalid_user_    = 0;
    uint64_t                                   accepted_        = 0;
    std::unordered_map<std::string, uint64_t>  ip_hits_;
    std::unordered_map<std::string, uint64_t>  user_hits_;
};

} // namespace budyk
