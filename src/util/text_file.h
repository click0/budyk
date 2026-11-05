// SPDX-License-Identifier: BSD-3-Clause
//
/// @file
/// A whole text file into a string: the rules file (Lua or YAML) and
/// what else the daemon reads at once. Regular files only, bounded.
#pragma once
#include <cstddef>
#include <string>

namespace budyk {

/// The largest file read_text_file() reads (16 MiB): a rules file is a
/// few kilobytes, and a path that names something else (a device, a
/// stray dump) must not be read to the end.
constexpr size_t kTextFileMax = size_t{16} << 20;

/// Read the file at `path` whole into `*text`. 0, or -errno with the
/// reason in `*error` (when given): a directory is -EISDIR, a FIFO, a
/// device or a socket -ENOTSUP "not a regular file" (opening a FIFO
/// used to hold the daemon until something wrote to it, and a directory
/// read as a file of 2^63 bytes), one over `kTextFileMax` -EFBIG.
int read_text_file(const char* path, std::string* text, std::string* error = nullptr);

} // namespace budyk
