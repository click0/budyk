// SPDX-License-Identifier: BSD-3-Clause
// The terminal-free parts of the TUI: HTTP reply parsing, the session
// cookie, password quoting for the login body, and field lookup in an
// /api/samples body.
#include "tui/tui.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace budyk::tui_detail;

int main() {
    // 1. parse_http_reply: status, headers and body are split out.
    {
        HttpReply r;
        assert(parse_http_reply(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: 2\r\n\r\n{}", &r));
        assert(r.status == 200);
        assert(r.headers.find("Content-Type: application/json") != std::string::npos);
        assert(r.body == "{}");

        assert(parse_http_reply("HTTP/1.1 401 Unauthorized\r\n\r\n", &r));
        assert(r.status == 401 && r.body.empty());

        assert(!parse_http_reply("", &r));
        assert(!parse_http_reply("HTTP/1.1 200 OK\r\nno end of headers", &r));
        assert(!parse_http_reply("garbage\r\n\r\n", &r));
        assert(!parse_http_reply("HTTP/1.1 abc\r\n\r\n", &r));
        assert(!parse_http_reply("HTTP/1.1 999 Nope\r\n\r\n", &r));
    }

    // 2. session_cookie: the value the daemon's login handler sets, with
    //    its attributes dropped; the header name is case-insensitive;
    //    other cookies and an empty value are ignored.
    {
        assert(session_cookie(
            "Content-Type: application/json\r\n"
            "Set-Cookie: budyk_session=abc123; HttpOnly; Path=/; SameSite=Strict")
               == "budyk_session=abc123");
        assert(session_cookie("set-cookie: budyk_session=xyz") == "budyk_session=xyz");
        assert(session_cookie("Set-Cookie: other=1\r\nSet-Cookie: budyk_session=t2; Path=/")
               == "budyk_session=t2");
        assert(session_cookie("Set-Cookie: budyk_session=; Max-Age=0").empty());
        assert(session_cookie("X-Set-Cookie: budyk_session=no").empty());
        assert(session_cookie("").empty());
    }

    // 3. json_quote: a password with quotes, backslashes or control
    //    characters still makes a valid JSON string.
    {
        assert(json_quote("pw") == "\"pw\"");
        assert(json_quote("a\"b\\c") == "\"a\\\"b\\\\c\"");
        assert(json_quote("t\ta\nb") == "\"t\\ta\\nb\"");
        assert(json_quote(std::string("\x01", 1)) == "\"\\u0001\"");
        assert(json_quote("пароль") == "\"пароль\"");      // UTF-8 passes through
    }

    // 4. sample_number reads the newest sample, within the named section.
    //    Regression: the TUI used to take the last "total" in the body,
    //    which is proc.total, and showed the process count as the memory
    //    total.
    {
        const std::string body =
            "{\"count\":2,\"samples\":["
            "{\"ts\":1,\"level\":1,\"cpu\":{\"total_percent\":10.0,\"count\":4},"
            "\"mem\":{\"total\":1000,\"available\":500,\"available_percent\":50.0},"
            "\"proc\":{\"total\":11,\"running\":1},\"uptime_seconds\":100.0},"
            "{\"ts\":2,\"level\":3,\"cpu\":{\"total_percent\":55.5,\"count\":8},"
            "\"mem\":{\"total\":16877547520,\"available\":4000,\"available_percent\":25.0},"
            "\"swap\":{\"total\":0,\"used\":0,\"used_percent\":0.0},"
            "\"proc\":{\"total\":105,\"running\":2},\"uptime_seconds\":200.5}"
            "]}";
        assert(sample_number(body, "mem", "total") == 16877547520.0);   // not 105
        assert(sample_number(body, "proc", "total") == 105.0);
        assert(sample_number(body, "cpu", "count") == 8.0);             // not top-level count
        assert(sample_number(body, "cpu", "total_percent") == 55.5);    // newest sample
        assert(sample_number(body, nullptr, "uptime_seconds") == 200.5);
        // Missing section / key → fallback, never a value from a neighbour.
        assert(sample_number(body, "thermal", "max_celsius", -1.0) == -1.0);
        assert(sample_number(body, "swap", "available", -1.0) == -1.0);
        assert(sample_number("{\"error\":\"unauthenticated\"}", "mem", "total", -1.0) == -1.0);
        assert(sample_number("", "mem", "total", -1.0) == -1.0);
    }

    std::printf("test_tui: PASS\n");
    return 0;
}
