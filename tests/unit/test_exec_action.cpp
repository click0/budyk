// SPDX-License-Identifier: BSD-3-Clause
// Exercises the fork/exec/timeout helper against real /bin programs.

#include "rules/exec_action.h"

#include <cassert>
#include <cstdio>
#include <unistd.h>

using namespace budyk;

static bool exists(const char* path) {
    return access(path, X_OK) == 0;
}

int main() {
    const char* TRUE_BIN  = exists("/bin/true")  ? "/bin/true"
                          : exists("/usr/bin/true") ? "/usr/bin/true" : nullptr;
    const char* FALSE_BIN = exists("/bin/false") ? "/bin/false"
                          : exists("/usr/bin/false") ? "/usr/bin/false" : nullptr;
    const char* SLEEP_BIN = exists("/bin/sleep") ? "/bin/sleep"
                          : exists("/usr/bin/sleep") ? "/usr/bin/sleep" : nullptr;

    // 1. Null / degenerate arguments rejected.
    {
        ExecResult r{};
        assert(exec_command(nullptr, 1, &r)         != 0);
        const char* empty[] = {nullptr};
        assert(exec_command(empty, 1, &r)           != 0);
        const char* ok[] = {"true", nullptr};
        assert(exec_command(ok, 0, &r)              != 0);   // timeout ≤ 0
        assert(exec_command(ok, -1, &r)             != 0);
        assert(exec_command(ok, 1, nullptr)         != 0);
    }

    // 2. /bin/true → exit_status == 0, not signalled, not timed out.
    if (TRUE_BIN) {
        const char* argv[] = {TRUE_BIN, nullptr};
        ExecResult r{};
        int rc = exec_command(argv, 5, &r);
        assert(rc == 0);
        assert(r.exit_status     == 0);
        assert(r.signal          == 0);
        assert(r.timed_out       == false);
        assert(r.elapsed_seconds >= 0.0);
        assert(r.elapsed_seconds <  5.0);
    }

    // 3. /bin/false → exit_status == 1.
    if (FALSE_BIN) {
        const char* argv[] = {FALSE_BIN, nullptr};
        ExecResult r{};
        int rc = exec_command(argv, 5, &r);
        assert(rc == 0);
        assert(r.exit_status == 1);
        assert(r.timed_out   == false);
    }

    // 4. /bin/sleep 10 with timeout=1 → timed_out == true, SIGKILL'd.
    if (SLEEP_BIN) {
        const char* argv[] = {SLEEP_BIN, "10", nullptr};
        ExecResult r{};
        int rc = exec_command(argv, 1, &r);
        assert(rc == 0);
        assert(r.timed_out       == true);
        assert(r.signal          == 9);    // SIGKILL
        assert(r.elapsed_seconds >= 1.0);
        assert(r.elapsed_seconds <  3.0);  // generous upper bound
    }

    // 5. Non-existent binary → child exits 127 after execvp failure.
    {
        const char* argv[] = {"/nonexistent/path/xyzzy", nullptr};
        ExecResult r{};
        int rc = exec_command(argv, 5, &r);
        assert(rc == 0);
        assert(r.exit_status == 127);
        assert(r.timed_out   == false);
    }

    std::printf("test_exec_action: PASS\n");
    return 0;
}
