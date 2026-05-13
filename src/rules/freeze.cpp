// SPDX-License-Identifier: BSD-3-Clause
#include "rules/freeze.h"

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(BUDYK_FREEBSD)
extern "C" {
#include <sys/sysctl.h>
#include <sys/user.h>
}
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace budyk {

int proc_name_of(int pid, char* buf, std::size_t cap) {
    if (buf == nullptr || cap == 0 || pid <= 0) return -1;
    buf[0] = '\0';

#if defined(BUDYK_LINUX)
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return -1;
    if (std::fgets(buf, static_cast<int>(cap), f) == nullptr) {
        std::fclose(f);
        return -1;
    }
    std::fclose(f);
    // Strip trailing newline(s).
    std::size_t n = std::strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    return n == 0 ? -1 : 0;

#elif defined(BUDYK_FREEBSD)
    int            mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    struct kinfo_proc kp{};
    std::size_t       sz  = sizeof(kp);
    if (::sysctl(mib, 4, &kp, &sz, nullptr, 0) != 0 || sz == 0) return -1;
    std::strncpy(buf, kp.ki_comm, cap - 1);
    buf[cap - 1] = '\0';
    return buf[0] == '\0' ? -1 : 0;

#else
    (void)pid;
    return -1;
#endif
}

int freeze_pid(int pid) {
    if (pid <= 0) return -EINVAL;
    if (::kill(static_cast<pid_t>(pid), SIGSTOP) != 0) return -errno;
    return 0;
}

int unfreeze_pid(int pid) {
    if (pid <= 0) return -EINVAL;
    if (::kill(static_cast<pid_t>(pid), SIGCONT) != 0) return -errno;
    return 0;
}

} // namespace budyk
