// SPDX-License-Identifier: BSD-3-Clause
#pragma once

namespace budyk {

// Outcome of a completed or killed child process.
struct ExecResult {
    int    exit_status;       // WEXITSTATUS value if the child exited normally
    int    signal;            // WTERMSIG value if the child was signalled
    bool   timed_out;         // true → we SIGKILL'd on timeout expiry
    double elapsed_seconds;
};

// Spawn argv[0] with argv as arguments under a sandbox:
//   * new process group (kill -pgid on timeout),
//   * stdin / stdout / stderr redirected to /dev/null,
//   * RLIMIT_CPU  = timeout_seconds + 5 / RLIMIT_AS = 256 MiB.
//
// Parent polls waitpid(WNOHANG) every 10 ms; if the deadline passes the
// child (and its pgid) is SIGKILL'd and reaped.
//
// Returns 0 on success (the child was spawned and eventually terminated —
// check `out` for its fate). Negative on setup failure before fork/exec.
//
// argv must be NULL-terminated; argv[0] is the program (execvp-style —
// resolved via PATH unless it contains a slash).
int exec_command(const char* const argv[],
                 int timeout_seconds,
                 ExecResult* out);

} // namespace budyk
