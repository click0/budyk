// SPDX-License-Identifier: BSD-3-Clause
// Alert dispatcher — payload builders + bookkeeping. The actual
// network POST path goes through curl(1) via popen and is exercised
// manually; here we only assert the request bodies are well-formed.

#include "rules/alert.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace budyk;

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // 1. severity_name maps the three enum values + falls back safely.
    {
        assert(std::string(severity_name(AlertSeverity::Info))     == "info");
        assert(std::string(severity_name(AlertSeverity::Warning))  == "warning");
        assert(std::string(severity_name(AlertSeverity::Critical)) == "critical");
    }

    // 2. ntfy_payload — the body is just the message text. Title /
    //    priority / tags ride in headers, exercised via the dispatcher
    //    plumbing rather than the payload itself.
    {
        const std::string body =
            ntfy_payload(AlertSeverity::Critical, "memory_low", "RAM is gone");
        assert(body == "RAM is gone");
    }

    // 3. discord_payload — JSON with embed: title, description, colour.
    {
        const std::string body = discord_payload(
            AlertSeverity::Warning, "high_cpu", "cpu.total_percent > 95");
        assert(contains(body, "\"embeds\":["));
        assert(contains(body, "\"title\":\"[warning] high_cpu\""));
        assert(contains(body, "\"description\":\"cpu.total_percent > 95\""));
        assert(contains(body, "\"color\":16753920"));   // orange = warning
    }

    // 4. discord_payload — escapes JSON-hostile chars in the message.
    {
        const std::string body = discord_payload(
            AlertSeverity::Critical, "rule",
            "tab\there\nnew\"line");
        assert(contains(body, "\\t"));
        assert(contains(body, "\\n"));
        assert(contains(body, "\\\""));      // escaped double-quote
        assert(contains(body, "\"color\":15158332"));   // red = critical
    }

    // 5. AlertDispatcher — empty dispatcher dispatches to zero channels.
    {
        AlertDispatcher d;
        assert(d.channel_count() == 0);
        const int ok = d.dispatch(AlertSeverity::Info, "rule", "msg");
        assert(ok == 0);
    }

    // 6. AlertDispatcher — unknown channel type is rejected (rc=0 sent
    //    successes; logged to stderr otherwise).
    {
        AlertDispatcher d;
        AlertChannel ch;
        ch.name = "ops";
        ch.type = "carrier-pigeon";
        ch.url  = "https://example.invalid/";
        d.add_channel(std::move(ch));
        assert(d.channel_count() == 1);
        const int ok = d.dispatch(AlertSeverity::Warning, "x", "y");
        assert(ok == 0);             // unknown type → not counted
    }

    // 7. AlertDispatcher — unreachable channel: dispatcher does not
    //    crash, returns 0 successes, the count stays as configured.
    //    We use a port that's almost certainly closed (127.0.0.1:1)
    //    and a 10-second curl timeout, which means this test takes a
    //    moment but still terminates.
    {
        AlertDispatcher d;
        AlertChannel ch;
        ch.name = "unreach";
        ch.type = "ntfy";
        ch.url  = "http://127.0.0.1:1";
        ch.topic = "x";
        d.add_channel(std::move(ch));
        const int ok = d.dispatch(AlertSeverity::Info, "x", "y");
        assert(ok == 0);
        assert(d.channel_count() == 1);
    }

    std::printf("test_alert: PASS\n");
    return 0;
}
