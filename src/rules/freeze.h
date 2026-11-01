// SPDX-License-Identifier: BSD-3-Clause
//
/// @file
/// PID freeze/unfreeze — sends SIGSTOP / SIGCONT to a PID, plus a
/// helper that resolves the process name (`comm`) for allowlist matching.
///
/// Used by the Lua `freeze()` / `unfreeze()` actions in lua_stdlib.cpp.
/// All entry points are best-effort: they wrap `kill(2)` and return
/// -errno on failure so the caller can surface it to the rule.

#pragma once
#include <cstddef>

namespace budyk {

/// Resolve the executable / kernel-reported `comm` for `pid`. On Linux
/// reads `/proc/<pid>/comm` (15-char kernel-thread name, trailing \n
/// stripped); on FreeBSD pulls `kinfo_proc.ki_comm` via sysctl.
///
/// Returns 0 on success (buf NUL-terminated), -1 on any failure (PID
/// gone, permission denied, malformed value). `cap` must be > 0.
int proc_name_of(int pid, char* buf, std::size_t cap);

/// True for the pids freeze_pid() and unfreeze_pid() refuse whatever the
/// allowlist says: budyk's own (a rule over `proc.top` with budyk at the
/// top SIGSTOPped the daemon, and nothing ever sent SIGCONT) and init.
bool freeze_refused(int pid);

/// kill(pid, SIGSTOP) — returns 0 on success, -errno on failure (-EPERM
/// for a pid freeze_refused()).
int freeze_pid(int pid);

/// kill(pid, SIGCONT) — returns 0 on success, -errno on failure.
int unfreeze_pid(int pid);

} // namespace budyk
