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

    // 7. telegram_payload — JSON with chat_id + "[sev] name: message".
    {
        const std::string body = telegram_payload(
            AlertSeverity::Info, "-100123456789", "uptime_ok",
            "uptime > 86400");
        assert(contains(body, "\"chat_id\":\"-100123456789\""));
        assert(contains(body, "\"text\":\"[info] uptime_ok: uptime > 86400\""));
    }

    // 8. telegram_payload — empty message: no trailing ": " separator.
    {
        const std::string body = telegram_payload(
            AlertSeverity::Critical, "42", "alarm", "");
        assert(contains(body, "\"text\":\"[critical] alarm\""));
    }

    // 9. smtp_message — required headers + body all present.
    {
        const std::string m = smtp_message(
            AlertSeverity::Warning,
            "alerts@example.com", "oncall@example.com",
            "high_load", "load_1m=12.0");
        assert(contains(m, "From: alerts@example.com\r\n"));
        assert(contains(m, "To: oncall@example.com\r\n"));
        assert(contains(m, "Subject: [budyk:warning] high_load\r\n"));
        assert(contains(m, "Date: "));
        assert(contains(m, "MIME-Version: 1.0\r\n"));
        assert(contains(m, "Content-Type: text/plain; charset=utf-8\r\n"));
        assert(contains(m, "\r\n\r\nload_1m=12.0\r\n"));   // blank line + body
    }

    // 10. twilio_form — URL-encoded From/To/Body. '+' in E.164 phone
    //     numbers must be encoded as %2B, '%' in the body as %25,
    //     spaces in the body as '+', ':' as %3A.
    {
        const std::string b = twilio_form(
            "+15551234567", "+15559876543",
            "high_cpu", "cpu pegged at 99%");
        assert(contains(b, "From=%2B15551234567"));
        assert(contains(b, "To=%2B15559876543"));
        assert(contains(b, "Body=%5Bbudyk%5D+high_cpu%3A+cpu+pegged+at+99%25"));
    }

    // 11. AlertDispatcher — smtp channel missing required fields is
    //     skipped (not counted, dispatcher returns 0 successes).
    {
        AlertDispatcher d;
        AlertChannel ch;
        ch.name = "broken-smtp";
        ch.type = "smtp";
        ch.url  = "smtp://localhost:25";
        // from + topic deliberately missing
        d.add_channel(std::move(ch));
        const int ok = d.dispatch(AlertSeverity::Warning, "x", "y");
        assert(ok == 0);
    }

    // 12. AlertDispatcher — twilio channel missing token is skipped.
    {
        AlertDispatcher d;
        AlertChannel ch;
        ch.name  = "broken-twilio";
        ch.type  = "twilio";
        ch.url   = "https://api.twilio.com/2010-04-01/Accounts/AC.../Messages.json";
        ch.from  = "+15551234567";
        ch.topic = "+15559876543";
        // token deliberately missing
        d.add_channel(std::move(ch));
        const int ok = d.dispatch(AlertSeverity::Warning, "x", "y");
        assert(ok == 0);
    }

    // 13. AlertDispatcher — unreachable channel: dispatcher does not
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
