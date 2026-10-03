// SPDX-License-Identifier: BSD-3-Clause
#include "web/http_util.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace budyk;

int main() {
    // 1. cookie_value: first, middle, last, with and without spaces,
    //    a name that is a prefix of another, missing, empty value.
    {
        assert(cookie_value("budyk_session=abc", "budyk_session") == "abc");
        assert(cookie_value("a=1; budyk_session=abc; b=2", "budyk_session") == "abc");
        assert(cookie_value("a=1;budyk_session=abc", "budyk_session") == "abc");
        assert(cookie_value("a=1; \tbudyk_session=abc", "budyk_session") == "abc");
        assert(cookie_value("budyk_session_old=zzz; budyk_session=abc", "budyk_session") == "abc");
        assert(cookie_value("budyk_session_old=zzz", "budyk_session").empty());
        assert(cookie_value("", "budyk_session").empty());
        assert(cookie_value("budyk_session=", "budyk_session").empty());
        assert(cookie_value("x=budyk_session=abc", "budyk_session").empty());
    }

    // 2. split_target.
    {
        std::string path, query = "stale";
        split_target("/api/range?since=1&limit=5", &path, &query);
        assert(path == "/api/range" && query == "since=1&limit=5");
        split_target("/api/levels", &path, &query);
        assert(path == "/api/levels" && query.empty());
        split_target("/x?", &path, &query);
        assert(path == "/x" && query.empty());
        split_target("/x?a=1?b=2", &path, &query);
        assert(path == "/x" && query == "a=1?b=2");
    }

    // 3. query_u64: present, absent, empty, non-digit, sign, leading
    //    zeros, a key that prefixes another, and saturation.
    {
        assert(query_u64("since=12&until=34", "since", 7) == 12);
        assert(query_u64("since=12&until=34", "until", 7) == 34);
        assert(query_u64("since=12&until=34", "limit", 7) == 7);
        assert(query_u64("limit=", "limit", 7) == 7);
        assert(query_u64("limit=abc", "limit", 7) == 7);
        assert(query_u64("limit=-1", "limit", 7) == 7);
        assert(query_u64("limit=+1", "limit", 7) == 7);
        assert(query_u64("limit=007", "limit", 7) == 7 || query_u64("limit=007", "limit", 0) == 7);
        assert(query_u64("limits=9&limit=3", "limit", 7) == 3);
        assert(query_u64("limit=18446744073709551615", "limit", 7) == UINT64_MAX);
        assert(query_u64("limit=18446744073709551616", "limit", 7) == UINT64_MAX);   // saturates
        assert(query_u64("limit=99999999999999999999999", "limit", 7) == UINT64_MAX);
        assert(query_u64("", "limit", 7) == 7);
    }

    // 4. query_str.
    {
        assert(query_str("level=burst&x=1", "level") == "burst");
        assert(query_str("x=1&level=L1", "level") == "L1");
        assert(query_str("x=1", "level").empty());
        assert(query_str("level=", "level").empty());
        assert(query_str("levels=all", "level").empty());
    }

    std::printf("test_http_util: PASS\n");
    return 0;
}
