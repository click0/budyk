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
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace budyk;

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The daemon's log is stderr: what the dispatcher says about a channel
// is read back through a file that stands in for it meanwhile.
struct StderrCapture {
    int         saved = -1;
    std::string path;
    void begin() {
        char tmpl[] = "/tmp/budyk_alert_log_XXXXXX";
        const int fd = ::mkstemp(tmpl);
        assert(fd >= 0);
        path  = tmpl;
        saved = ::dup(2);
        std::fflush(stderr);
        ::dup2(fd, 2);
        ::close(fd);
    }
    std::string end() {
        std::fflush(stderr);
        ::dup2(saved, 2);
        ::close(saved);
        std::string out;
        FILE* f = std::fopen(path.c_str(), "r");
        assert(f != nullptr);
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
        ::unlink(path.c_str());
        return out;
    }
};

// An executable `name` in one of $PATH's directories.
static bool on_path(const char* name) {
    const char* path = ::getenv("PATH");
    std::string dirs = path != nullptr ? path : "";
    for (size_t at = 0; at <= dirs.size(); ) {
        size_t end = dirs.find(':', at);
        if (end == std::string::npos) end = dirs.size();
        const std::string file = dirs.substr(at, end - at) + "/" + name;
        if (end > at && ::access(file.c_str(), X_OK) == 0) return true;
        at = end + 1;
    }
    return false;
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

    // 9b. A line break in the rule name (anyone's bytes: a process name,
    //     a mount point) stays inside its header: it used to start a
    //     header of its own, a Bcc: in the mail or any ntfy header.
    {
        const std::string evil = "disk /mnt/x\r\nBcc: everyone@example.com\nX-Evil: 1";
        const std::string m = smtp_message(AlertSeverity::Warning, "a@example.com",
                                           "b@example.com", evil, "body");
        assert(contains(m, "Subject: [budyk:warning] disk /mnt/x  Bcc: everyone@example.com X-Evil: 1\r\n"));
        assert(!contains(m, "\r\nBcc:") && !contains(m, "\nX-Evil"));
        const auto h = ntfy_headers(AlertSeverity::Critical, evil);
        assert(h.size() == 3);
        assert(h[0] == "Title: budyk: disk /mnt/x  Bcc: everyone@example.com X-Evil: 1");
        for (const auto& line : h) assert(line.find_first_of("\r\n") == std::string::npos);
        assert(header_value(std::string("a\x01" "b\x7f" "c\t")) == "a b c ");
        assert(header_value("caf\xc3\xa9") == "caf\xc3\xa9");   // UTF-8 kept
    }

    // 9c. The body's line ends are CR LF whatever the message had: curl
    //     dot-stuffs a line that starts with "." only after CR LF, so a
    //     bare-LF "\n.\n" (from a process name, anyone's bytes) went out
    //     as it was, and an MTA that takes <LF>.<LF> as the end of DATA
    //     cut the mail there and read what followed as commands.
    {
        const std::string m = smtp_message(AlertSeverity::Warning, "a@example.com",
                                           "b@example.com", "r", "line1\n.\nQUIT\r\nline3\rend");
        assert(contains(m, "\r\n\r\nline1\r\n.\r\nQUIT\r\nline3\r\nend\r\n"));
        assert(!contains(m, "\n.\nQUIT") && !contains(m, "\r\r\n") && !contains(m, "\rend"));
        const std::string plain = smtp_message(AlertSeverity::Info, "a@x", "b@x", "r", "one\r\ntwo");
        assert(contains(plain, "\r\n\r\none\r\ntwo\r\n"));
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

    // 15. Secrets never reach curl's argv, which any local user can read
    //     through ps(1): the Telegram bot token sits in the URL path, a
    //     Discord webhook URL is the credential, Twilio and SMTP carry
    //     "user:pass". A stand-in `curl` first in PATH records its argv
    //     and the files it was pointed at (they are unlinked once curl
    //     exits, so it has to read them while it runs). The token must
    //     be absent from argv and present in a 0600 -K config file, and
    //     a quote in the URL must be escaped, not end the value.
    if (::access("/bin/sh", X_OK) == 0) {
        char dir_tmpl[] = "/tmp/budyk_fakecurl_XXXXXX";
        const char* dir = ::mkdtemp(dir_tmpl);
        assert(dir != nullptr);
        const std::string fake = std::string(dir) + "/curl";
        const std::string rec  = std::string(dir) + "/record";
        {
            FILE* f = std::fopen(fake.c_str(), "w");
            assert(f != nullptr);
            std::fprintf(f,
                "#!/bin/sh\n"
                "printf 'ARGV:' >> '%s'\n"
                "for a in \"$@\"; do printf ' [%%s]' \"$a\" >> '%s'; done\n"
                "printf '\\n' >> '%s'\n"
                "while [ $# -gt 0 ]; do\n"
                "  if [ \"$1\" = -K ] || [ \"$1\" = --netrc-file ]; then\n"
                "    printf 'FILE %%s mode=%%s: ' \"$1\" \"$(stat -c %%a \"$2\" 2>/dev/null || stat -f %%Lp \"$2\")\" >> '%s'\n"
                "    cat \"$2\" >> '%s'\n"
                "  fi\n"
                "  if [ \"$1\" = -D ]; then printf 'HTTP/1.1 200 OK\\r\\n\\r\\n' > \"$2\"; fi\n"
                "  shift\n"
                "done\n"
                "exit 0\n",
                rec.c_str(), rec.c_str(), rec.c_str(), rec.c_str(), rec.c_str());
            std::fclose(f);
            ::chmod(fake.c_str(), 0755);
        }
        const char* old_path = ::getenv("PATH");
        const std::string new_path = std::string(dir) + ":" + (old_path != nullptr ? old_path : "");
        ::setenv("PATH", new_path.c_str(), 1);

        AlertDispatcher d;
        auto add = [&](const char* type, const std::string& url, const std::string& from,
                       const std::string& topic, const std::string& token) {
            AlertChannel ch;
            ch.name = type; ch.type = type; ch.url = url;
            ch.from = from; ch.topic = topic; ch.token = token;
            d.add_channel(std::move(ch));
        };
        add("telegram", "", "", "42", "BOT_TOKEN_SECRET");                 // token in the URL path
        add("discord",  "https://discord.example/api/webhooks/WEBHOOK_SECRET?x=\"q\"", "", "", "");
        add("twilio",   "https://api.twilio.example/Accounts/AC1/Messages.json",
                        "+1", "+2", "AC1:TWILIO_SECRET");
        add("smtp",     "smtps://mail.example:465", "a@x", "b@x", "mailuser:MAIL_SECRET");
        assert(d.dispatch(AlertSeverity::Info, "r", "m") == 4);
        assert(d.flush(30000));
        assert(d.delivered() == 4);                   // the stand-in exits 0

        std::string record;
        {
            FILE* f = std::fopen(rec.c_str(), "r");
            assert(f != nullptr);
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) record.append(buf, n);
            std::fclose(f);
        }
        // argv lines carry no secret and no URL...
        std::string argv_lines, file_lines;
        for (size_t pos = 0; pos < record.size(); ) {
            const size_t eol = record.find('\n', pos);
            const std::string line = record.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            (line.compare(0, 5, "ARGV:") == 0 ? argv_lines : file_lines) += line + "\n";
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        assert(argv_lines.find("BOT_TOKEN_SECRET") == std::string::npos);
        assert(argv_lines.find("WEBHOOK_SECRET")   == std::string::npos);
        assert(argv_lines.find("TWILIO_SECRET")    == std::string::npos);
        assert(argv_lines.find("MAIL_SECRET")      == std::string::npos);
        assert(argv_lines.find("--url")            == std::string::npos);
        assert(argv_lines.find("https://")         == std::string::npos);
        assert(argv_lines.find("smtps://")         == std::string::npos);
        // ...the files do, and are private to the daemon.
        assert(file_lines.find("url = \"https://api.telegram.org/botBOT_TOKEN_SECRET/sendMessage\"") != std::string::npos);
        assert(file_lines.find("url = \"https://discord.example/api/webhooks/WEBHOOK_SECRET?x=\\\"q\\\"\"") != std::string::npos);
        assert(file_lines.find("user = \"AC1:TWILIO_SECRET\"")      != std::string::npos);
        assert(file_lines.find("user = \"mailuser:MAIL_SECRET\"")   != std::string::npos);
        assert(argv_lines.find("--netrc-file") == std::string::npos);
        // The body as it is: -d @file drops its line breaks.
        assert(argv_lines.find(" [-d] ") == std::string::npos);
        assert(argv_lines.find(" [--data-binary] [@") != std::string::npos);
        assert(file_lines.find("mode=600") != std::string::npos);
        assert(file_lines.find("mode=644") == std::string::npos);

        ::setenv("PATH", old_path != nullptr ? old_path : "", 1);
        ::unlink(fake.c_str());
        ::unlink(rec.c_str());
        ::rmdir(dir);
    }

    // 15b. What curl says when it fails reaches the log: its stderr went
    //      to /dev/null with the child's, and the operator got
    //      "failed (rc=-3)" for a DNS failure, a TLS failure, a timeout
    //      and curl not installed alike. A stand-in exits 6 with curl's
    //      words in the --stderr file; another is not there at all.
    if (::access("/bin/sh", X_OK) == 0) {
        char dir_tmpl[] = "/tmp/budyk_fakecurl_XXXXXX";
        const char* dir = ::mkdtemp(dir_tmpl);
        assert(dir != nullptr);
        const std::string fake = std::string(dir) + "/curl";
        {
            FILE* f = std::fopen(fake.c_str(), "w");
            assert(f != nullptr);
            std::fputs("#!/bin/sh\n"
                       "while [ $# -gt 0 ]; do\n"
                       "  if [ \"$1\" = --stderr ]; then echo 'curl: (6) Could not resolve host: nope' > \"$2\"; fi\n"
                       "  shift\n"
                       "done\n"
                       "exit 6\n", f);
            std::fclose(f);
            ::chmod(fake.c_str(), 0755);
        }
        const char* old_path = ::getenv("PATH");
        const std::string new_path = std::string(dir) + ":" + (old_path != nullptr ? old_path : "");
        ::setenv("PATH", new_path.c_str(), 1);

        AlertDispatcher d;
        AlertChannel ch;
        ch.name = "n"; ch.type = "ntfy"; ch.url = "https://nope.example"; ch.topic = "t";
        d.add_channel(std::move(ch));
        StderrCapture log;
        log.begin();
        assert(d.dispatch(AlertSeverity::Info, "r", "m") == 1);
        assert(d.flush(30000));
        std::string text = log.end();
        assert(d.failed() == 1);
        assert(contains(text, "budyk alert: channel 'n' (ntfy) failed: "
                              "curl: (6) Could not resolve host: nope (exit 6)\n"));

        ::unlink(fake.c_str());                       // no curl in this directory now
        ::setenv("PATH", dir, 1);                     // ...and nowhere else
        AlertDispatcher none;
        AlertChannel ch2;
        ch2.name = "n"; ch2.type = "ntfy"; ch2.url = "https://nope.example"; ch2.topic = "t";
        none.add_channel(std::move(ch2));
        log.begin();
        assert(none.dispatch(AlertSeverity::Info, "r", "m") == 1);
        assert(none.flush(30000));
        text = log.end();
        assert(none.failed() == 1);
        assert(contains(text, "failed: curl not found in PATH (exit 127)\n"));

        ::setenv("PATH", old_path != nullptr ? old_path : "", 1);
        ::rmdir(dir);
    }

    // 16. What a server gets from the real curl(1), when it is installed:
    //     an ntfy message keeps its line breaks (-d @file dropped them:
    //     "/var 97% fulllargest: ..."), and a Twilio token whose password
    //     has blanks (a Gmail app password as Google shows it) is sent
    //     whole (netrc split it: Basic auth of "AC1:abcd"). The third
    //     channel is answered 400 as Telegram answers a wrong chat_id:
    //     curl exits 0 on that, and it counted as delivered, with no log
    //     line; it is a failure now, with the status line and the body's
    //     reason in the log.
    if (on_path("curl")) {
        int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(lfd >= 0);
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::bind(lfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
        assert(::listen(lfd, 4) == 0);
        socklen_t len = sizeof(sa);
        assert(::getsockname(lfd, reinterpret_cast<sockaddr*>(&sa), &len) == 0);
        const std::string base = "http://127.0.0.1:" + std::to_string(ntohs(sa.sin_port));

        std::vector<std::string> requests;
        std::thread server([&] {
            for (int i = 0; i < 3; ++i) {
                const int c = ::accept(lfd, nullptr, nullptr);
                if (c < 0) return;
                std::string req;
                char buf[4096];
                size_t want = std::string::npos;     // header block + body
                while (req.size() < want) {
                    const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    req.append(buf, static_cast<size_t>(n));
                    const size_t end = req.find("\r\n\r\n");
                    if (want == std::string::npos && end != std::string::npos) {
                        const size_t cl = req.find("Content-Length: ");
                        want = end + 4 + (cl != std::string::npos && cl < end
                            ? std::stoul(req.substr(cl + 16)) : 0);
                    }
                }
                const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                                   "Content-Length: 73\r\nConnection: close\r\n\r\n"
                                   "{\"ok\":false,\"error_code\":400,\"description\":\"Bad Request: chat not found\"}";
                if (contains(req, "POST /bot")) {
                    (void)::send(c, bad, sizeof(bad) - 1, MSG_NOSIGNAL);
                } else {
                    (void)::send(c, ok, sizeof(ok) - 1, MSG_NOSIGNAL);
                }
                ::close(c);
                requests.push_back(req);
            }
        });

        AlertDispatcher d;
        AlertChannel ntfy;
        ntfy.name = "n"; ntfy.type = "ntfy"; ntfy.url = base; ntfy.topic = "t";
        d.add_channel(std::move(ntfy));
        AlertChannel tw;
        tw.name = "w"; tw.type = "twilio"; tw.url = base + "/tw";
        tw.from = "+1"; tw.topic = "+2"; tw.token = "AC1:abcd efgh ijkl";
        d.add_channel(std::move(tw));
        AlertChannel tg;
        tg.name = "tg"; tg.type = "telegram"; tg.url = base + "/bot"; tg.topic = "42"; tg.token = "T";
        d.add_channel(std::move(tg));
        StderrCapture log;
        log.begin();
        assert(d.dispatch(AlertSeverity::Warning, "disk_full", "/var 97% full\nlargest: log") == 3);
        assert(d.flush(30000));
        const std::string text = log.end();
        server.join();
        ::close(lfd);
        assert(requests.size() == 3);
        assert(d.delivered() == 2 && d.failed() == 1);
        assert(contains(requests[0], "\r\n\r\n/var 97% full\nlargest: log"));
        assert(contains(requests[1], "Authorization: Basic QUMxOmFiY2QgZWZnaCBpamts\r\n"));
        assert(contains(requests[2], "\"chat_id\":\"42\""));
        assert(contains(text, "budyk alert: channel 'tg' (telegram) failed: HTTP/1.1 400 Bad Request: "
                              "{\"ok\":false,\"error_code\":400,\"description\":\"Bad Request: chat not found\"}\n"));
        assert(!contains(text, "channel 'n'") && !contains(text, "channel 'w'"));
    }

    std::printf("test_alert: PASS\n");
    return 0;
}
