// SPDX-License-Identifier: BSD-3-Clause
// freeze.cpp exercises `kill(2)` directly; the test forks a tiny child
// that just calls `pause(2)`, freezes it, asserts WIFSTOPPED via
// waitpid(WUNTRACED), unfreezes it, then reaps. Linux + FreeBSD both
// honour SIGSTOP/SIGCONT identically here.

#include "rules/freeze.h"

#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

using namespace budyk;

int main() {
    // 1. proc_name_of(getpid()) — should return our own argv[0] basename
    //    ("test_freeze"); kernel truncates to 15 chars so the prefix is
    //    enough to verify.
    {
        char buf[64] = {0};
        const int rc = proc_name_of(static_cast<int>(::getpid()),
                                    buf, sizeof(buf));
        assert(rc == 0);
        assert(buf[0] != '\0');
        // Loose match — kernel may report "test_freeze" or "lt-test…"
        // depending on the libtool wrapper. Just ensure it's nonempty
        // and printable.
        for (char c : std::string(buf)) {
            assert(static_cast<unsigned char>(c) >= 0x20);
        }
    }

    // 2. proc_name_of(0) and negative pids are rejected.
    {
        char buf[16];
        assert(proc_name_of(0,  buf, sizeof(buf)) == -1);
        assert(proc_name_of(-1, buf, sizeof(buf)) == -1);
    }

    // 3. freeze + unfreeze a forked child that's blocked in pause(2).
    //    waitpid(WUNTRACED) returns once the child is SIGSTOP-paused,
    //    so we get a synchronous confirmation that the signal landed.
    {
        const pid_t child = ::fork();
        assert(child >= 0);
        if (child == 0) {
            // Child: block here until SIGTERM (sent by parent post-test).
            for (;;) ::pause();
            _exit(0);
        }

        // Tiny settle delay so the child reaches pause(2) before we
        // signal it. SIGSTOP works on a runnable process too, but
        // waitpid(WUNTRACED) below races on a still-forking child.
        ::usleep(50 * 1000);

        assert(freeze_pid(child) == 0);

        int status = 0;
        const pid_t got = ::waitpid(child, &status, WUNTRACED);
        assert(got == child);
        assert(WIFSTOPPED(status));
        assert(WSTOPSIG(status) == SIGSTOP);

        assert(unfreeze_pid(child) == 0);

        // Tear the child down cleanly.
        ::kill(child, SIGTERM);
        ::waitpid(child, &status, 0);
    }

    // 4. freeze a non-existent PID — kernel returns ESRCH; our wrapper
    //    surfaces it as -ESRCH.
    {
        // PID 0x7FFFFFFE is virtually guaranteed to be unallocated on
        // both Linux (default pid_max = 4M) and FreeBSD (PID_MAX 99999).
        const int rc = freeze_pid(0x7FFFFFFE);
        assert(rc == -ESRCH || rc == -EPERM);
    }

    // 5. unfreeze a non-existent PID — same surface.
    {
        const int rc = unfreeze_pid(0x7FFFFFFE);
        assert(rc == -ESRCH || rc == -EPERM);
    }

    std::printf("test_freeze: PASS\n");
    return 0;
}
