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
#include <cstring>
#include <string>
#include <vector>

extern char** environ;

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

    // The program starts with the default signal dispositions and none
    // blocked. The daemon ignores SIGPIPE (a client that hangs up), and an
    // ignored signal stays ignored across execve: a rule's script got
    // EPIPE where `producer | head -1` ends the producer quietly. SIGXFSZ
    // likewise (budyk ignores it to see EFBIG).
    signal(SIGPIPE, SIG_DFL);
    signal(SIGXFSZ, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);

    // Detach stdio — we don't want the child scribbling on our logs.
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0) _exit(127);
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > 2) close(devnull);

    // CPU time cap — SIGXCPU fires at rlim_cur, SIGKILL at rlim_max.
    struct rlimit cpu {};
    // Widen before adding: timeout_seconds + 5 in int could overflow.
    const rlim_t t = static_cast<rlim_t>(timeout_seconds > 0 ? timeout_seconds : 1);
    cpu.rlim_cur = t + 5;
    cpu.rlim_max = t + 10;
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

int wait_with_timeout(pid_t pid, int timeout_seconds, ExecResult* out,
                      const std::atomic<bool>* cancel) {
    const double start    = monotonic_seconds();
    const double deadline = start + static_cast<double>(timeout_seconds);

    // Kill the whole process group, then reap.
    auto kill_and_reap = [&]() {
        kill(-pid, SIGKILL);
        kill( pid, SIGKILL);
        int s = 0;
        // A SIGHUP or SIGTERM taken by this thread (no SA_RESTART) ended
        // the wait with EINTR, and the child was never reaped: a zombie
        // for the daemon's life.
        while (waitpid(pid, &s, 0) < 0 && errno == EINTR) {}
        out->signal          = WIFSIGNALED(s) ? WTERMSIG(s) : 0;
        out->elapsed_seconds = monotonic_seconds() - start;
    };

    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            out->elapsed_seconds = monotonic_seconds() - start;
            if (WIFEXITED(status))       out->exit_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) out->signal     = WTERMSIG(status);
            return 0;
        }
        if (r < 0 && errno != EINTR) { out->err = errno; return -5; }

        if (cancel != nullptr && cancel->load()) {
            kill_and_reap();
            out->cancelled = true;
            return 0;
        }
        if (monotonic_seconds() >= deadline) {
            kill_and_reap();
            out->timed_out = true;
            return 0;
        }

        struct timespec ts{};
        ts.tv_sec  = 0;
        ts.tv_nsec = 10 * 1000 * 1000;   // 10 ms poll interval
        nanosleep(&ts, nullptr);
    }
}

} // namespace

namespace {

// The environment a child gets: what a command needs to run and reach
// the network, and nothing else of the daemon's. A `budyk serve` from an
// admin's shell, `sudo -E` or an env-rich container handed every exec()
// command and curl the shell's tokens (155 variables in a test run).
bool child_env_keeps(const char* name) {
    static const char* const kKeep[] = {
        "PATH", "HOME", "LANG", "TZ", "TMPDIR",
        "http_proxy", "https_proxy", "ftp_proxy", "all_proxy", "no_proxy",
        "HTTP_PROXY", "HTTPS_PROXY", "FTP_PROXY", "ALL_PROXY", "NO_PROXY",
        "CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR",
    };
    for (const char* k : kKeep) {
        if (std::strcmp(name, k) == 0) return true;
    }
    return std::strncmp(name, "LC_", 3) == 0;
}

std::vector<std::string> child_environment() {
    std::vector<std::string> env;
    bool path = false;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const char* eq = std::strchr(*e, '=');
        if (eq == nullptr) continue;
        const std::string name(*e, static_cast<size_t>(eq - *e));
        if (!child_env_keeps(name.c_str())) continue;
        env.emplace_back(*e);
        if (name == "PATH") path = true;
    }
    if (!path) env.emplace_back("PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin");
    return env;
}

} // namespace

const char* exec_describe(int rc) {
    switch (rc) {
        case 0:  return "ok";
        case -1: return "no command";
        case -2: return "no timeout";
        case -3: return "no result slot";
        case -4: return "fork";
        case -5: return "waitpid";
        default: return "unknown error";
    }
}

int exec_command(const char* const argv[], int timeout_seconds, ExecResult* out,
                 const std::atomic<bool>* cancel) {
    if (argv == nullptr || argv[0] == nullptr) return -1;
    if (timeout_seconds <= 0)                  return -2;
    if (out == nullptr)                        return -3;

    *out = ExecResult{};

    // Made before the fork: the child must not allocate.
    const std::vector<std::string> env = child_environment();
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (const auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) { out->err = errno; return -4; }
    if (pid == 0) {
        environ = envp.data();   // execvp() reads PATH from here
        prepare_child_and_exec(argv, timeout_seconds);
    }

    return wait_with_timeout(pid, timeout_seconds, out, cancel);
}

} // namespace budyk
