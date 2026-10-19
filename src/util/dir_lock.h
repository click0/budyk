// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>

namespace budyk {

/// @file
/// One budyk at a time may write a data_dir: two daemons on the same
/// rings would interleave their records. The lock is an flock(2) on
/// `<dir>/budyk.lock`, which the kernel drops when the holder exits in any
/// way (SIGKILL included), so a stale file never blocks a restart.

/// Takes the lock without waiting: creates the file (0600) if needed and
/// writes this process's pid into it. Returns the descriptor to keep open
/// for as long as the lock is wanted (close-on-exec), or -errno:
/// -EWOULDBLOCK when another process holds it, with what that process
/// wrote (its pid) in *holder.
int lock_data_dir(const char* dir, std::string* holder);

/// Whether another process holds the lock, without taking it (fsck when
/// it only reads). False when there is no lock file. *holder as above.
bool data_dir_locked(const char* dir, std::string* holder);

} // namespace budyk
