// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <string>

namespace budyk {

// Write `body` to a fresh private temporary file and return its path in
// `path_out` (cap bytes). The file is created by mkstemp(3), so it is
// mode 0600 and never pre-existing; it lives in $TMPDIR when set, else
// /tmp, and is named <prefix>XXXXXX. Used for what must not travel on a
// command line: curl bodies, header files, netrc and -K config files,
// LLM prompts. The caller unlinks it. False on any failure (the path
// is then not meaningful).
bool write_private_tmp(const char* prefix, const std::string& body,
                       char* path_out, size_t cap);

} // namespace budyk
