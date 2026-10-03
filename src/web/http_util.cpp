// SPDX-License-Identifier: BSD-3-Clause
#include "web/http_util.h"

#include <cctype>
#include <cstring>

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

namespace {

// The raw value of `key` in "a=1&b=2", or false when absent. Shared by
// query_u64 and query_str.
bool query_find(const std::string& query, const char* key, std::string* value) {
    const std::string needle = std::string(key) + "=";
    size_t p = 0;
    while (p < query.size()) {
        size_t amp = query.find('&', p);
        if (amp == std::string::npos) amp = query.size();
        if (query.compare(p, needle.size(), needle) == 0) {
            *value = query.substr(p + needle.size(), amp - p - needle.size());
            return true;
        }
        p = amp + 1;
    }
    return false;
}

} // namespace

uint64_t query_u64(const std::string& query, const char* key, uint64_t fallback) {
    std::string val;
    if (!query_find(query, key, &val) || val.empty()) return fallback;
    uint64_t out = 0;
    for (char c : val) {
        if (c < '0' || c > '9') return fallback;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (out > (UINT64_MAX - digit) / 10) return UINT64_MAX;   // saturate
        out = out * 10 + digit;
    }
    return out;
}

std::string query_str(const std::string& query, const char* key) {
    std::string val;
    return query_find(query, key, &val) ? val : std::string();
}

bool ascii_ieq(const std::string& a, const char* b) {
    const size_t blen = std::strlen(b);
    if (a.size() != blen) return false;
    for (size_t i = 0; i < blen; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool ascii_icontains(const std::string& hay, const char* needle) {
    const size_t nlen = std::strlen(needle);
    if (nlen == 0) return true;
    for (size_t i = 0; i + nlen <= hay.size(); ++i) {
        size_t k = 0;
        while (k < nlen &&
               std::tolower(static_cast<unsigned char>(hay[i + k])) ==
               std::tolower(static_cast<unsigned char>(needle[k]))) {
            ++k;
        }
        if (k == nlen) return true;
    }
    return false;
}

} // namespace budyk
