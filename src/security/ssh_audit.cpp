// SPDX-License-Identifier: BSD-3-Clause
#include "security/ssh_audit.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace budyk {

namespace {

// Returns the substring from `s` starting at `pos`, ending at the
// first whitespace, ':' (port suffix in some formats), or end of
// string. Empty input → empty output.
std::string extract_token(const std::string& s, std::size_t pos) {
    std::size_t end = pos;
    while (end < s.size()) {
        const char c = s[end];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == ':' || c == ',') break;
        ++end;
    }
    return s.substr(pos, end - pos);
}

// Find "from <ip>" anywhere after `pos`. Returns empty on miss.
std::string find_ip_after(const std::string& s, std::size_t pos) {
    const std::size_t from = s.find(" from ", pos);
    if (from == std::string::npos) return {};
    return extract_token(s, from + 6);
}

// Cap on distinct keys we'll remember — keeps memory bounded under a
// flood of distinct attacker IPs / usernames. When the table hits this
// size, new keys are dropped (existing keys still increment).
constexpr std::size_t kMaxDistinctKeys = 4096;

void bump(std::unordered_map<std::string, uint64_t>& m, const std::string& k) {
    if (k.empty()) return;
    auto it = m.find(k);
    if (it != m.end()) {
        ++it->second;
    } else if (m.size() < kMaxDistinctKeys) {
        m.emplace(k, 1);
    }
    // else: silently drop — flood protection.
}

} // namespace

SshAuditScanner::SshAuditScanner() = default;

void SshAuditScanner::reset() {
    offset_          = 0;
    failed_password_ = 0;
    invalid_user_    = 0;
    accepted_        = 0;
    ip_hits_.clear();
    user_hits_.clear();
}

uint64_t SshAuditScanner::offset() const { return offset_; }

void SshAuditScanner::ingest_line(const std::string& line) {
    // The three patterns we care about all sit after sshd's syslog
    // prefix, so search for the marker substrings rather than parsing
    // from the start — works across syslog, journalctl, and the
    // `sshd[NNN]:` variants.
    const std::size_t failed_pos  = line.find("Failed password");
    const std::size_t invalid_pos = line.find("Invalid user");
    const std::size_t accepted_pos =
        line.find("Accepted password") != std::string::npos
            ? line.find("Accepted password")
            : line.find("Accepted publickey");

    if (failed_pos != std::string::npos) {
        ++failed_password_;
        // Username — after "for ". sshd may insert "invalid user " between
        // "for " and the name when the account doesn't exist, so we
        // step past that variant.
        std::size_t user_pos = line.find(" for ", failed_pos);
        if (user_pos != std::string::npos) {
            user_pos += 5;
            // Skip the optional "invalid user " infix.
            if (line.compare(user_pos, 13, "invalid user ") == 0) {
                user_pos += 13;
            }
            bump(user_hits_, extract_token(line, user_pos));
        }
        bump(ip_hits_, find_ip_after(line, failed_pos));
    } else if (invalid_pos != std::string::npos) {
        ++invalid_user_;
        const std::size_t user_pos = invalid_pos + 13;  // "Invalid user "
        bump(user_hits_, extract_token(line, user_pos));
        bump(ip_hits_,   find_ip_after(line, invalid_pos));
    } else if (accepted_pos != std::string::npos) {
        ++accepted_;
        // Successful logins aren't offenders, so we don't tally their
        // ip / user into the top-N tables.
    }
}

int SshAuditScanner::scan(const char* path, SshAuditStats* out) {
    if (path == nullptr) return -EINVAL;

    struct stat st{};
    if (::stat(path, &st) != 0) {
        // Missing file isn't a hard error — a fresh box may have no
        // auth.log yet. Just return the current snapshot.
        if (out != nullptr) *out = snapshot();
        return 0;
    }

    // Log rotation / truncation: file is now smaller than what we
    // already consumed → start from 0.
    if (static_cast<uint64_t>(st.st_size) < offset_) {
        offset_ = 0;
    }

    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return -errno;

    if (std::fseek(f, static_cast<long>(offset_), SEEK_SET) != 0) {
        std::fclose(f);
        return -errno;
    }

    // 8K is plenty for any sshd log line; if a line is longer (very
    // rare), fgets will return it in chunks — we just over-count.
    char buf[8192];
    while (std::fgets(buf, sizeof(buf), f) != nullptr) {
        ingest_line(buf);
    }
    offset_ = static_cast<uint64_t>(std::ftell(f));
    std::fclose(f);

    if (out != nullptr) *out = snapshot();
    return 0;
}

void SshAuditScanner::recompute_top(
    std::vector<std::pair<std::string, uint64_t>>& out,
    const std::unordered_map<std::string, uint64_t>& src) const {
    out.assign(src.begin(), src.end());
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    constexpr std::size_t kTopN = 16;
    if (out.size() > kTopN) out.resize(kTopN);
}

SshAuditStats SshAuditScanner::snapshot() const {
    SshAuditStats s;
    s.failed_password = failed_password_;
    s.invalid_user    = invalid_user_;
    s.accepted        = accepted_;
    recompute_top(s.top_ips,   ip_hits_);
    recompute_top(s.top_users, user_hits_);
    return s;
}

} // namespace budyk
