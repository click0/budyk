// SPDX-License-Identifier: BSD-3-Clause
// Alert dispatcher — payload builders + bookkeeping. The actual
// network POST path goes through curl(1) via popen and is exercised
// manually; here we only assert the request bodies are well-formed.

#include "rules/alert.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

    // 13. AlertDispatcher — a channel that is down. First a port that
    //    is closed (127.0.0.1:1): the attempt fails, the dispatcher
    //    does not crash, the count stays as configured. Then a server
    //    that accepts and never answers, which is what a stuck channel
    //    looks like: dispatch() still returns at once because the send
    //    runs on the dispatcher's thread, and stop() cancels the
    //    running curl instead of waiting out its --max-time.
    {
        AlertDispatcher d;
        AlertChannel ch;
        ch.name = "unreach";
        ch.type = "ntfy";
        ch.url  = "http://127.0.0.1:1";
        ch.topic = "x";
        d.add_channel(std::move(ch));
        assert(d.dispatch(AlertSeverity::Info, "x", "y") == 1);
        assert(d.channel_count() == 1);
        assert(d.flush(30000));
        assert(d.delivered() == 0 && d.failed() == 1 && d.dropped() == 0);
        d.stop(30000);
        assert(d.dispatch(AlertSeverity::Info, "x", "late") == 0);   // stopped
        assert(d.dropped() == 1);
    }
    {
        // Listen, never accept: the connection completes in the kernel
        // backlog and curl waits for a response that never comes.
        int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(lfd >= 0);
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::bind(lfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
        assert(::listen(lfd, 4) == 0);
        socklen_t len = sizeof(sa);
        assert(::getsockname(lfd, reinterpret_cast<sockaddr*>(&sa), &len) == 0);

        AlertDispatcher d;
        AlertChannel ch;
        ch.name  = "stuck";
        ch.type  = "ntfy";
        ch.url   = "http://127.0.0.1:" + std::to_string(ntohs(sa.sin_port));
        ch.topic = "x";
        d.add_channel(std::move(ch));

        auto t0 = std::chrono::steady_clock::now();
        assert(d.dispatch(AlertSeverity::Critical, "x", "z") == 1);
        double secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        assert(secs < 1.0);                         // did not wait for curl
        assert(!d.flush(300));                      // curl is still waiting

        t0 = std::chrono::steady_clock::now();
        d.stop(200);                                // cancels the running curl
        secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        assert(secs < 3.0);                         // not curl's 10 s --max-time
        assert(d.delivered() == 0 && d.failed() == 1);
        ::close(lfd);
    }

    // 14. Channel fields never reach a shell. curl used to run through
    //     system() with the URL unquoted and SMTP addresses in single
    //     quotes, so a `;`, `$(...)` or `'` in the config ran commands as
    //     the daemon. Each backend gets fields that would `touch` a marker
    //     file through a shell; the file must not appear. (This holds
    //     whether or not curl is installed: the shell ran the touch
    //     either way.)
    {
        char dir_tmpl[] = "/tmp/budyk_inj_XXXXXX";
        const char* dir = ::mkdtemp(dir_tmpl);
        assert(dir != nullptr);
        const std::string marker = std::string(dir) + "/PWNED";
        const std::string inject = ";touch " + marker + ";";
        const std::string quoted = "x@y.com'; touch " + marker + "; echo '";

        AlertDispatcher d;
        auto add = [&](const char* type, const std::string& url,
                       const std::string& from, const std::string& topic,
                       const std::string& token) {
            AlertChannel ch;
            ch.name = type; ch.type = type; ch.url = url;
            ch.from = from; ch.topic = topic; ch.token = token;
            d.add_channel(std::move(ch));
        };
        add("ntfy",    "http://127.0.0.1:1/" + inject, "", "t", "");
        add("discord", "http://127.0.0.1:1/$(touch " + marker + ")", "", "", "");
        add("twilio",  "http://127.0.0.1:1/" + inject, "+1", "+2", "AC:" + inject);
        add("smtp",    "smtp://127.0.0.1:1", quoted, quoted, "");
        add("ntfy",    "-o" + marker, "", "t", "");   // not an option either
        assert(d.dispatch(AlertSeverity::Info, "r", "m") == 5);
        assert(d.flush(60000));                       // the sends have run

        assert(::access(marker.c_str(), F_OK) != 0);
        ::unlink(marker.c_str());
        ::rmdir(dir);
    }

    std::printf("test_alert: PASS\n");
    return 0;
}
