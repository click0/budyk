// SPDX-License-Identifier: BSD-3-Clause
// SSH brute-force scanner — exercise the line parser, the offset
// bookkeeping (incremental scans), and log-rotation handling.

#include "security/ssh_audit.h"

#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace budyk;

// Write `content` (truncating any previous content) to a temp file
// and return its path. Caller is responsible for unlink()ing.
static std::string write_tmp(const char* content) {
    char path[64];
    std::strcpy(path, "/tmp/budyk_ssh_XXXXXX");
    int fd = ::mkstemp(path);
    assert(fd >= 0);
    const std::size_t n = std::strlen(content);
    const ssize_t w = ::write(fd, content, n);
    assert(w == static_cast<ssize_t>(n));
    ::close(fd);
    return std::string(path);
}

static void overwrite(const std::string& path, const char* content) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    assert(f != nullptr);
    std::fputs(content, f);
    std::fclose(f);
}

int main() {
    // 1. ingest_line counts each pattern correctly.
    {
        SshAuditScanner s;
        s.ingest_line("Apr 17 10:23:01 host sshd[1234]: Failed password for root from 198.51.100.7 port 33333 ssh2");
        s.ingest_line("Apr 17 10:23:02 host sshd[1235]: Invalid user admin from 198.51.100.7 port 33334");
        s.ingest_line("Apr 17 10:23:03 host sshd[1235]: Failed password for invalid user admin from 198.51.100.7 port 33334 ssh2");
        s.ingest_line("Apr 17 10:24:01 host sshd[1236]: Accepted publickey for jane from 203.0.113.4 port 56789 ssh2");
        s.ingest_line("Apr 17 10:25:01 host sshd[1237]: Failed password for root from 192.0.2.55 port 12345 ssh2");

        const auto st = s.snapshot();
        assert(st.failed_password == 3);
        assert(st.invalid_user    == 1);
        assert(st.accepted        == 1);

        // top_ips: 198.51.100.7 has 3 hits (2 failed + 1 invalid),
        // 192.0.2.55 has 1.
        assert(st.top_ips.size() >= 2);
        assert(st.top_ips[0].first  == "198.51.100.7");
        assert(st.top_ips[0].second == 3);
        assert(st.top_ips[1].first  == "192.0.2.55");
        assert(st.top_ips[1].second == 1);

        // top_users: root (2), admin (2 — once via Invalid user, once
        // via Failed password for invalid user). Tie broken by alpha
        // order — "admin" < "root".
        assert(st.top_users.size() >= 2);
        assert(st.top_users[0].first  == "admin");
        assert(st.top_users[0].second == 2);
        assert(st.top_users[1].first  == "root");
        assert(st.top_users[1].second == 2);
    }

    // 2. Accepted lines don't poison the top-N tables.
    {
        SshAuditScanner s;
        s.ingest_line("Accepted publickey for jane from 203.0.113.4 port 22 ssh2");
        const auto st = s.snapshot();
        assert(st.failed_password == 0);
        assert(st.accepted        == 1);
        assert(st.top_ips.empty());
        assert(st.top_users.empty());
    }

    // 3. scan() — fresh file, single shot.
    {
        const std::string p = write_tmp(
            "Failed password for root from 1.2.3.4 port 22 ssh2\n"
            "Invalid user foo from 5.6.7.8 port 22\n");

        SshAuditScanner s;
        SshAuditStats   st;
        assert(s.scan(p.c_str(), &st) == 0);
        assert(st.failed_password == 1);
        assert(st.invalid_user    == 1);
        assert(s.offset() > 0);
        ::unlink(p.c_str());
    }

    // 4. scan() picks up new lines on the next call without re-counting.
    {
        const std::string p = write_tmp(
            "Failed password for root from 1.1.1.1 port 22 ssh2\n");
        SshAuditScanner s;
        SshAuditStats   st;
        assert(s.scan(p.c_str(), &st) == 0);
        assert(st.failed_password == 1);

        // Append a new line.
        std::FILE* f = std::fopen(p.c_str(), "a");
        assert(f != nullptr);
        std::fputs("Failed password for root from 2.2.2.2 port 22 ssh2\n", f);
        std::fclose(f);

        assert(s.scan(p.c_str(), &st) == 0);
        assert(st.failed_password == 2);   // cumulative, not delta
        ::unlink(p.c_str());
    }

    // 5. Log rotation — file shrinks below cached offset, scanner
    //    rewinds to 0 and counts the new content.
    {
        const std::string p = write_tmp(
            "Failed password for root from 1.1.1.1 port 22 ssh2\n"
            "Failed password for root from 1.1.1.1 port 22 ssh2\n"
            "Failed password for root from 1.1.1.1 port 22 ssh2\n");
        SshAuditScanner s;
        SshAuditStats   st;
        assert(s.scan(p.c_str(), &st) == 0);
        assert(st.failed_password == 3);
        const uint64_t before = s.offset();
        assert(before > 0);

        // Rotate: truncate + write a single fresh line.
        overwrite(p, "Invalid user newcomer from 9.9.9.9 port 22\n");
        assert(s.scan(p.c_str(), &st) == 0);
        // Counters are cumulative; failed_password is still 3, plus
        // we picked up the new invalid_user line.
        assert(st.failed_password == 3);
        assert(st.invalid_user    == 1);
        assert(s.offset() < before);   // rewound
        ::unlink(p.c_str());
    }

    // 6. Missing path is not a hard error — returns 0 with no new
    //    counts so callers can probe a fresh box.
    {
        SshAuditScanner s;
        SshAuditStats   st;
        const int rc = s.scan("/tmp/budyk_ssh_definitely_does_not_exist", &st);
        assert(rc == 0);
        assert(st.failed_password == 0);
    }

    // 7. reset() wipes everything.
    {
        SshAuditScanner s;
        s.ingest_line("Failed password for root from 1.2.3.4 port 22 ssh2");
        assert(s.snapshot().failed_password == 1);
        s.reset();
        assert(s.snapshot().failed_password == 0);
        assert(s.offset() == 0);
    }

    // 8. NULL path is rejected with -EINVAL.
    {
        SshAuditScanner s;
        const int rc = s.scan(nullptr, nullptr);
        assert(rc < 0);
    }

    std::printf("test_ssh_audit: PASS\n");
    return 0;
}
