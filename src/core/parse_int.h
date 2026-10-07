// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace budyk {

// The whole of `s` as a base-10 integer in [lo, hi]: an optional sign
// and digits, nothing before or after them. False (and *out untouched)
// for an empty string, blanks, a unit or other trailing text, or a
// value out of range — where atoi() would quietly return 0 or a
// truncated number. For command-line and config values.
inline bool parse_int_full(const char* s, long lo, long hi, long* out) {
    if (s == nullptr) return false;
    const unsigned char first = static_cast<unsigned char>(*s);
    if (std::isdigit(first) == 0 && first != '-' && first != '+') return false;
    errno = 0;
    char* end = nullptr;
    const long n = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || n < lo || n > hi) return false;
    *out = n;
    return true;
}

} // namespace budyk
