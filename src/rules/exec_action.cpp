// SPDX-License-Identifier: BSD-3-Clause
#include "rules/exec_action.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace budyk {

namespace {

double monotonic_seconds() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) / 1.0e9;
}

// Executed only in the child. Must not allocate, must not throw.
void prepare_child_and_exec(const char* const argv[], int timeout_seconds) {
    // New process group so the parent can kill -pgid on timeout.
    setpgid(0, 0);

    // Detach stdio — we don't want the child scribbling on our logs.
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0) _exit(127);
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > 2) close(devnull);

    // CPU time cap — SIGXCPU fires at rlim_cur, SIGKILL at rlim_max.
    struct rlimit cpu {};
    cpu.rlim_cur = static_cast<rlim_t>(timeout_seconds + 5);
    cpu.rlim_max = static_cast<rlim_t>(timeout_seconds + 10);
    setrlimit(RLIMIT_CPU, &cpu);

    // Address space cap — 256 MiB is plenty for a shell-out action, and
    // short-circuits runaway allocators.
    struct rlimit as {};
    as.rlim_cur = 256ULL * 1024 * 1024;
    as.rlim_max = 256ULL * 1024 * 1024;
    setrlimit(RLIMIT_AS, &as);

    execvp(argv[0], const_cast<char* const*>(argv));
    _exit(127);   // execvp only returns on failure
}

int wait_with_timeout(pid_t pid, int timeout_seconds, ExecResult* out) {
    const double start    = monotonic_seconds();
    const double deadline = start + static_cast<double>(timeout_seconds);

    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            out->elapsed_seconds = monotonic_seconds() - start;
            if (WIFEXITED(status))       out->exit_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) out->signal     = WTERMSIG(status);
            return 0;
        }
        if (r < 0 && errno != EINTR) return -5;

        if (monotonic_seconds() >= deadline) {
            // Kill the whole process group, then reap.
            kill(-pid, SIGKILL);
            kill( pid, SIGKILL);
            int s = 0;
            waitpid(pid, &s, 0);
            out->timed_out       = true;
            out->signal          = WIFSIGNALED(s) ? WTERMSIG(s) : 0;
            out->elapsed_seconds = monotonic_seconds() - start;
            return 0;
        }

        struct timespec ts{};
        ts.tv_sec  = 0;
        ts.tv_nsec = 10 * 1000 * 1000;   // 10 ms poll interval
        nanosleep(&ts, nullptr);
    }
}

} // namespace

int exec_command(const char* const argv[], int timeout_seconds, ExecResult* out) {
    if (argv == nullptr || argv[0] == nullptr) return -1;
    if (timeout_seconds <= 0)                  return -2;
    if (out == nullptr)                        return -3;

    *out = ExecResult{};

    pid_t pid = fork();
    if (pid < 0)  return -4;
    if (pid == 0) prepare_child_and_exec(argv, timeout_seconds);

    return wait_with_timeout(pid, timeout_seconds, out);
}

} // namespace budyk
