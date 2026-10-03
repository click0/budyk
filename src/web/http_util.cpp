// SPDX-License-Identifier: BSD-3-Clause
#include "web/http_util.h"

namespace budyk {

// Extract the value of a single cookie name from a Cookie header line
// like "a=1; b=2". Returns empty string if missing.
std::string cookie_value(const std::string& cookie_header, const char* name) {
    const std::string needle = std::string(name) + "=";
    size_t p = 0;
    while (p < cookie_header.size()) {
        size_t end = cookie_header.find(';', p);
        if (end == std::string::npos) end = cookie_header.size();
        size_t start = p;
        while (start < end && (cookie_header[start] == ' ' || cookie_header[start] == '\t'))
            ++start;
        if (cookie_header.compare(start, needle.size(), needle) == 0) {
            return cookie_header.substr(start + needle.size(), end - start - needle.size());
        }
        p = end + 1;
    }
    return {};
}

// Split a request target into path + raw query string. "/api/range?x=1"
// → path "/api/range", query "x=1". No query → query stays empty.
void split_target(const std::string& target,
                  std::string* path, std::string* query) {
    const size_t q = target.find('?');
    if (q == std::string::npos) {
        *path  = target;
        query->clear();
    } else {
        *path  = target.substr(0, q);
        *query = target.substr(q + 1);
    }
}

// Pull a single unsigned 64-bit value for `key` out of a urlencoded
// query string ("a=1&b=2"). Returns `fallback` when the key is absent
// or doesn't parse as a non-negative integer. Only digits are accepted
// — no signs, no units (callers pass nanoseconds / counts directly).
uint64_t query_u64(const std::string& query, const char* key, uint64_t fallback) {
    const std::string needle = std::string(key) + "=";
    size_t p = 0;
    while (p < query.size()) {
        size_t amp = query.find('&', p);
        if (amp == std::string::npos) amp = query.size();
        if (query.compare(p, needle.size(), needle) == 0) {
            const std::string val = query.substr(p + needle.size(),
                                                 amp - p - needle.size());
            if (val.empty()) return fallback;
            uint64_t out = 0;
            for (char c : val) {
                if (c < '0' || c > '9') return fallback;
                const uint64_t digit = static_cast<uint64_t>(c - '0');
                if (out > (UINT64_MAX - digit) / 10) return UINT64_MAX;   // saturate
                out = out * 10 + digit;
            }
            return out;
        }
        p = amp + 1;
    }
    return fallback;
}

// Raw value of `key` in a query string ("" if absent). No percent-decoding:
// callers only look up level names, which are [A-Za-z0-9_-].
std::string query_str(const std::string& query, const char* key) {
    const std::string needle = std::string(key) + "=";
    size_t p = 0;
    while (p < query.size()) {
        size_t amp = query.find('&', p);
        if (amp == std::string::npos) amp = query.size();
        if (query.compare(p, needle.size(), needle) == 0) {
            return query.substr(p + needle.size(), amp - p - needle.size());
        }
        p = amp + 1;
    }
    return std::string();
}

} // namespace budyk
