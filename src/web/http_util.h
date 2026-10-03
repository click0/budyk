// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <string>

namespace budyk {

// Small parsers the router uses on request data. Pure functions: no
// allocation beyond the returned string, no I/O, unit-tested in
// tests/unit/test_http_util.cpp.

// Value of one cookie in a Cookie header line like "a=1; b=2"; "" if
// missing.
std::string cookie_value(const std::string& cookie_header, const char* name);

// Split a request target into path + raw query string: "/api/range?x=1"
// → path "/api/range", query "x=1". No query → query is cleared.
void split_target(const std::string& target, std::string* path, std::string* query);

// Unsigned value of `key` in a urlencoded query ("a=1&b=2"). `fallback`
// when the key is absent, empty or not all digits; saturates at
// UINT64_MAX instead of wrapping.
uint64_t query_u64(const std::string& query, const char* key, uint64_t fallback);

// Raw value of `key` in a query string ("" if absent). No
// percent-decoding: callers only look up level names, [A-Za-z0-9_-].
std::string query_str(const std::string& query, const char* key);

} // namespace budyk
