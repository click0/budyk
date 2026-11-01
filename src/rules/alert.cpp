// SPDX-License-Identifier: BSD-3-Clause
#include "rules/alert.h"
#include "rules/exec_action.h"
#include "core/json_text.h"
#include "util/tmpfile.h"

#include <strings.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace budyk {

namespace {

// RFC 3986 unreserved URL-encode. Used for x-www-form-urlencoded
// bodies (Twilio). Keeps A-Za-z0-9-._~ verbatim; everything else is
// percent-escaped. Spaces become '+' (the form spec, not pure RFC 3986).
std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool unreserved =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
             c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Temp files for curl: bodies, header files, the -K url config, and the
// files curl writes (its stderr, the reply's headers and body). Private
// (0600) and unlinked once curl exits. False with `why` set.
bool write_tmp(const std::string& body, char* path_out, size_t cap, std::string* why) {
    if (write_private_tmp("budyk_alert_", body, path_out, cap)) return true;
    *why = std::string("cannot write a temp file: ") + std::strerror(errno);
    return false;
}

// Up to `cap` bytes of a file curl wrote; "" when it cannot be read.
std::string read_head(const char* path, size_t cap) {
    std::string out;
    std::FILE* f = std::fopen(path, "re");
    if (f == nullptr) return out;
    out.resize(cap);
    const size_t n = std::fread(&out[0], 1, cap, f);
    std::fclose(f);
    out.resize(n);
    return out;
}

// The first line of `text`, control characters as spaces, at most 200
// characters: curl's own words and a reply's body go into one log line.
std::string first_line(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\n') break;
        const auto u = static_cast<unsigned char>(c);
        out += (u < 0x20 || u == 0x7f) ? ' ' : c;
        if (out.size() >= 200) { out += "..."; break; }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// `key = "value"` for a curl config file, quoted per curl's config
// syntax: backslash, double quote and the control characters that would
// start a new line or option are escaped, so the value is one value
// whatever it contains.
std::string config_line(const char* key, const std::string& value) {
    std::string line = key;
    line += " = \"";
    for (const char c : value) {
        switch (c) {
            case '\\': line += "\\\\"; break;
            case '"':  line += "\\\""; break;
            case '\n': line += "\\n";  break;
            case '\r': line += "\\r";  break;
            case '\t': line += "\\t";  break;
            default:   line += c;      break;
        }
    }
    line += "\"\n";
    return line;
}

// A curl config file (-K) holding the URL, and the "user:password" when
// there is one, so neither appears in curl's argv: a Telegram bot token
// sits in the URL path and a Discord webhook URL is itself the
// credential, and argv is readable by every local user through ps(1).
// The file is 0600 from mkstemp and unlinked as soon as curl exits. The
// password went through a netrc file, which splits on blanks: a Gmail
// app password as Google shows it, "abcd efgh ijkl mnop", was sent as
// "abcd".
bool write_curl_config(const char* url, const std::string& user_pass,
                       char* path_out, size_t cap, std::string* why) {
    std::string cfg = config_line("url", url);
    if (!user_pass.empty()) cfg += config_line("user", user_pass);
    return write_tmp(cfg, path_out, cap, why);
}

const char* severity_color(AlertSeverity s) {
    // Discord embed colour as an int (decimal). Standard SOC colours.
    switch (s) {
        case AlertSeverity::Info:     return "3447003";   // blue
        case AlertSeverity::Warning:  return "16753920";  // orange
        case AlertSeverity::Critical: return "15158332";  // red
    }
    return "16753920";
}

const char* ntfy_priority(AlertSeverity s) {
    // ntfy.sh priority header: 1..5 (default 3).
    switch (s) {
        case AlertSeverity::Info:     return "3";
        case AlertSeverity::Warning:  return "4";
        case AlertSeverity::Critical: return "5";
    }
    return "3";
}

// Runs curl with `args` as its argv, directly (fork + execvp, no shell).
// A URL, address or token with spaces, quotes, `;` or `$(...)` stays one
// argument and is never parsed as shell syntax; the URL itself goes
// through a -K config file (write_curl_config), so it is neither an
// option nor visible in ps(1). stdio is /dev/null and the run is killed
// after timeout_s (see exec_command). True when curl exited 0; otherwise
// `why` says how it ended, in curl's own words where it has them (-sS
// to a --stderr file: "curl: (6) Could not resolve host: ..."). The
// operator used to get "failed (rc=-3)" for every one of them.
bool run_curl(const std::vector<std::string>& args, int timeout_s,
              const std::atomic<bool>* cancel, std::string* why) {
    char err_path[PATH_MAX];
    if (!write_tmp("", err_path, sizeof(err_path), why)) return false;
    std::vector<const char*> argv;
    argv.reserve(args.size() + 4);
    argv.push_back("curl");
    argv.push_back("--stderr");
    argv.push_back(err_path);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    ExecResult res{};
    const int rc  = exec_command(argv.data(), timeout_s, &res, cancel);
    const int err = errno;
    const std::string said = first_line(read_head(err_path, 4096));
    ::unlink(err_path);
    if (rc != 0) {
        *why = rc == -4 ? std::string("cannot fork curl: ") + std::strerror(err)
                        : std::string("cannot run curl");
        return false;
    }
    if (res.cancelled) {
        *why = "cancelled at shutdown";
    } else if (res.timed_out) {
        *why = "curl ran past " + std::to_string(timeout_s) + " s and was killed";
    } else if (res.signal != 0) {
        *why = "curl was killed by signal " + std::to_string(res.signal);
    } else if (res.exit_status == 127 && said.empty()) {
        *why = "curl not found in PATH (exit 127)";
    } else if (res.exit_status != 0) {
        *why = said.empty() ? "curl exited " + std::to_string(res.exit_status)
                            : said + " (exit " + std::to_string(res.exit_status) + ")";
    } else {
        return true;
    }
    return false;
}

// Was the HTTP reply a success? curl exits 0 on a 4xx or 5xx as well
// (no --fail: that would also drop the body, which says why: Telegram's
// "chat not found", Discord's "Unknown Webhook", ntfy's "forbidden", a
// 429), so the headers it dumped (-D) say whether the message was taken,
// and the body (-o) why not. Every such reply counted as delivered, and
// nothing was logged.
bool http_ok(const char* reply_hdr, const char* reply_body, std::string* why) {
    const std::string hdrs = read_head(reply_hdr, 16384);
    // The last status line: a "100 Continue" comes before the answer.
    size_t pos = hdrs.rfind("\nHTTP/");
    pos = pos == std::string::npos ? (hdrs.compare(0, 5, "HTTP/") == 0 ? 0 : std::string::npos)
                                   : pos + 1;
    if (pos == std::string::npos) {
        *why = "no HTTP status line in the reply";
        return false;
    }
    const std::string status = first_line(hdrs.substr(pos));
    const size_t sp = status.find(' ');
    const int code = sp != std::string::npos && status.size() >= sp + 4
        ? std::atoi(status.substr(sp + 1, 3).c_str()) : 0;   // NOLINT(cert-err34-c): curl's own line
    if (code >= 200 && code < 300) return true;
    *why = status;
    const std::string body = first_line(read_head(reply_body, 4096));
    if (!body.empty()) *why += ": " + body;
    return false;
}

// One HTTP POST: `body` with `extra_headers` to `url`, the reply checked.
// Both the body and the header lines go through files; --data-binary,
// because -d @file drops the file's CR, LF and NUL bytes, and an ntfy
// message, its body as it is, came out on one line. The "user:password"
// for Twilio rides in the -K config file as the URL does.
bool curl_post(const char* url,
               const std::string& user_pass,
               const std::string& body,
               const std::vector<std::string>& extra_headers,
               const std::atomic<bool>* cancel, std::string* why) {
    std::string headers;
    for (const auto& h : extra_headers) {
        headers += h;
        headers += "\n";
    }
    char body_path[PATH_MAX], hdr_path[PATH_MAX], cfg_path[PATH_MAX];
    char reply_hdr[PATH_MAX], reply_body[PATH_MAX];
    std::vector<const char*> made;
    auto cleanup = [&] { for (const char* p : made) ::unlink(p); };
    auto make = [&](const std::string& content, char* path) {
        if (!write_tmp(content, path, PATH_MAX, why)) return false;
        made.push_back(path);
        return true;
    };
    if (!make(body, body_path) || !make(headers, hdr_path) ||
        !make("", reply_hdr) || !make("", reply_body) ||
        !write_curl_config(url, user_pass, cfg_path, sizeof(cfg_path), why)) {
        cleanup();
        return false;
    }
    made.push_back(cfg_path);

    const bool ran = run_curl({"-sS", "-X", "POST",
                               "-H", std::string("@") + hdr_path,
                               "--data-binary", std::string("@") + body_path,
                               "-D", reply_hdr, "-o", reply_body,
                               "--max-time", "10", "-K", cfg_path}, 15, cancel, why);
    const bool ok = ran && http_ok(reply_hdr, reply_body, why);
    cleanup();
    return ok;
}

// SMTP delivery via curl. The credentials again ride in the -K config
// file (so neither the SMTP user nor password is visible in argv).
//   url: smtp://host:port or smtps://host:port
//   user_pass: "user:pass" (may be empty for unauth relays)
//   from/to: envelope sender + recipient
//   message: full RFC822 DATA blob (headers + blank line + body)
bool curl_smtp(const char* url,
               const std::string& user_pass,
               const std::string& from,
               const std::string& to,
               const std::string& message,
               const std::atomic<bool>* cancel, std::string* why) {
    char body_path[PATH_MAX];
    if (!write_tmp(message, body_path, sizeof(body_path), why)) return false;
    char cfg_path[PATH_MAX];
    if (!write_curl_config(url, user_pass, cfg_path, sizeof(cfg_path), why)) {
        ::unlink(body_path);
        return false;
    }
    const bool ok = run_curl({"-sS", "--mail-from", from, "--mail-rcpt", to,
                              "-T", body_path, "--max-time", "15", "-K", cfg_path},
                             20, cancel, why);
    ::unlink(body_path);
    ::unlink(cfg_path);
    return ok;
}

} // namespace

const char* severity_name(AlertSeverity s) {
    switch (s) {
        case AlertSeverity::Info:     return "info";
        case AlertSeverity::Warning:  return "warning";
        case AlertSeverity::Critical: return "critical";
    }
    return "warning";
}

bool parse_severity(const char* s, AlertSeverity* out) {
    if (s == nullptr || out == nullptr) return false;
    if (::strcasecmp(s, "info")     == 0) { *out = AlertSeverity::Info;     return true; }
    if (::strcasecmp(s, "warning")  == 0) { *out = AlertSeverity::Warning;  return true; }
    if (::strcasecmp(s, "critical") == 0) { *out = AlertSeverity::Critical; return true; }
    return false;
}

std::string header_value(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = ' ';
    }
    return out;
}

std::vector<std::string> ntfy_headers(AlertSeverity sev, const std::string& rule_name) {
    return {
        "Title: budyk: " + header_value(rule_name),
        std::string("Priority: ") + ntfy_priority(sev),
        std::string("Tags: ") + severity_name(sev),
    };
}

std::string ntfy_payload(AlertSeverity, const std::string&,
                         const std::string& message) {
    // ntfy.sh accepts a plain-text body — title / priority / tags are
    // all carried in HTTP headers. The body is just the message text.
    return message;
}

std::string telegram_payload(AlertSeverity sev, const std::string& chat_id,
                             const std::string& rule_name,
                             const std::string& message) {
    // Telegram Bot API sendMessage body. parse_mode left out — keep the
    // text plain to avoid having to escape Markdown / HTML.
    std::string out;
    out.reserve(message.size() + rule_name.size() + chat_id.size() + 64);
    out += "{\"chat_id\":\"";
    out += json_escape(chat_id);
    out += "\",\"text\":\"[";
    out += severity_name(sev);
    out += "] ";
    out += json_escape(rule_name);
    if (!message.empty()) {
        out += ": ";
        out += json_escape(message);
    }
    out += "\"}";
    return out;
}

std::string smtp_message(AlertSeverity sev, const std::string& from,
                         const std::string& to,
                         const std::string& rule_name,
                         const std::string& message) {
    // Compose a minimal RFC 5322 message. Date in IMF format because
    // some MTAs reject undated mail; %z gives the numeric offset.
    char date_buf[64];
    const std::time_t now = std::time(nullptr);
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &now);
#else
    localtime_r(&now, &tm_local);
#endif
    std::strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S %z", &tm_local);

    std::string out;
    out.reserve(message.size() + rule_name.size() + 256);
    out += "From: ";    out += header_value(from); out += "\r\n";
    out += "To: ";      out += header_value(to);   out += "\r\n";
    out += "Subject: [budyk:"; out += severity_name(sev); out += "] "; out += header_value(rule_name);
    out += "\r\n";
    out += "Date: ";    out += date_buf; out += "\r\n";
    out += "MIME-Version: 1.0\r\n";
    out += "Content-Type: text/plain; charset=utf-8\r\n";
    out += "\r\n";
    // The body's line ends as SMTP has them, CR LF: curl dot-stuffs a
    // line that starts with "." only when CR LF ends the one before, so
    // a bare-LF "\n.\n" in the message (a process name holds anyone's
    // bytes) went out as it was, and an MTA that takes <LF>.<LF> as the
    // end of DATA cut the mail there and read the rest as commands.
    for (size_t i = 0; i < message.size(); ++i) {
        const char c = message[i];
        if (c == '\r') {
            out += "\r\n";
            if (i + 1 < message.size() && message[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out += "\r\n";
        } else {
            out += c;
        }
    }
    out += "\r\n";
    return out;
}

std::string twilio_form(const std::string& from, const std::string& to,
                        const std::string& rule_name,
                        const std::string& message) {
    // SMS body: "[sev] rule: message" — Twilio truncates >1600 chars and
    // bills by 160-char segments; we don't try to be clever here.
    std::string text = "[budyk] ";
    text += rule_name;
    if (!message.empty()) {
        text += ": ";
        text += message;
    }
    std::string out;
    out.reserve(text.size() + from.size() + to.size() + 32);
    out += "From=" + url_encode(from);
    out += "&To="  + url_encode(to);
    out += "&Body="+ url_encode(text);
    return out;
}

std::string discord_payload(AlertSeverity sev, const std::string& rule_name,
                            const std::string& message) {
    // {"embeds":[{"title":"...","description":"...","color":N}]}
    std::string out;
    out.reserve(message.size() + rule_name.size() + 128);
    out += "{\"embeds\":[{";
    out += "\"title\":\"[";
    out += severity_name(sev);
    out += "] ";
    out += json_escape(rule_name);
    out += "\",";
    out += "\"description\":\"";
    out += json_escape(message);
    out += "\",";
    out += "\"color\":";
    out += severity_color(sev);
    out += "}]}";
    return out;
}

namespace {

// Is this channel configured well enough to send to? The same checks
// the send used to make, done before queueing so dispatch() can report
// the count of channels the alert will reach and log the rest once.
bool channel_complete(const AlertChannel& ch) {
    if (ch.type == "ntfy" || ch.type == "discord" || ch.type == "telegram") return true;
    if (ch.type == "smtp") {
        if (ch.from.empty() || ch.topic.empty() || ch.url.empty()) {
            std::fprintf(stderr,
                "budyk alert: smtp channel '%s' missing url/from/topic\n",
                ch.name.c_str());
            return false;
        }
        return true;
    }
    if (ch.type == "twilio") {
        if (ch.from.empty() || ch.topic.empty() || ch.url.empty() ||
            ch.token.empty()) {
            std::fprintf(stderr,
                "budyk alert: twilio channel '%s' missing url/token/from/topic\n",
                ch.name.c_str());
            return false;
        }
        return true;
    }
    std::fprintf(stderr,
        "budyk alert: unknown channel type '%s' on '%s'\n",
        ch.type.c_str(), ch.name.c_str());
    return false;
}

// One channel, one alert. Runs on the worker thread. True when the
// channel took the message; otherwise `why` is for the log.
bool send_one(const AlertChannel& ch, AlertSeverity sev,
              const std::string& rule_name, const std::string& message,
              const std::atomic<bool>* cancel, std::string* why) {
    if (ch.type == "ntfy") {
        // ntfy.sh: POST to <base>/<topic>, body = message,
        // headers: Title, Priority, Tags.
        std::string url = ch.url;
        if (!url.empty() && url.back() != '/') url.push_back('/');
        url += ch.topic;
        return curl_post(url.c_str(), "", ntfy_payload(sev, rule_name, message),
                         ntfy_headers(sev, rule_name), cancel, why);
    }
    if (ch.type == "discord") {
        return curl_post(ch.url.c_str(), "", discord_payload(sev, rule_name, message),
                         {"Content-Type: application/json"}, cancel, why);
    }
    if (ch.type == "telegram") {
        // url override; default to the public Bot API endpoint.
        std::string url = ch.url;
        if (url.empty()) {
            url  = "https://api.telegram.org/bot";
            url += ch.token;
            url += "/sendMessage";
        }
        return curl_post(url.c_str(), "", telegram_payload(sev, ch.topic, rule_name, message),
                         {"Content-Type: application/json"}, cancel, why);
    }
    if (ch.type == "smtp") {
        if (!ch.token.empty() && ch.token.find(':') == std::string::npos) {
            *why = "token is not \"user:password\"";
            return false;
        }
        return curl_smtp(ch.url.c_str(), ch.token, ch.from, ch.topic,
                         smtp_message(sev, ch.from, ch.topic, rule_name, message),
                         cancel, why);
    }
    if (ch.type == "twilio") {
        // Basic auth with "AccountSID:AuthToken", an x-www-form-urlencoded body.
        if (ch.token.find(':') == std::string::npos) {
            *why = "token is not \"AccountSID:AuthToken\"";
            return false;
        }
        return curl_post(ch.url.c_str(), ch.token, twilio_form(ch.from, ch.topic, rule_name, message),
                         {"Content-Type: application/x-www-form-urlencoded"}, cancel, why);
    }
    *why = "unknown channel type";
    return false;
}

} // namespace

void AlertDispatcher::add_channel(AlertChannel ch) {
    channels_.emplace_back(std::move(ch));
}

size_t AlertDispatcher::channel_count() const { return channels_.size(); }

uint64_t AlertDispatcher::dispatch_calls() const { return dispatch_calls_; }
const AlertDispatcher::Event& AlertDispatcher::last_event() const { return last_event_; }
uint64_t AlertDispatcher::delivered() const { return delivered_.load(); }
uint64_t AlertDispatcher::failed()    const { return failed_.load(); }
uint64_t AlertDispatcher::dropped()   const { return worker_.dropped(); }

bool AlertDispatcher::flush(int timeout_ms) { return worker_.wait_idle(timeout_ms); }
void AlertDispatcher::stop(int grace_ms)    { worker_.stop(grace_ms); }

int AlertDispatcher::dispatch(AlertSeverity sev,
                              const std::string& rule_name,
                              const std::string& message) {
    ++dispatch_calls_;
    last_event_.severity = sev;
    last_event_.rule     = rule_name;
    last_event_.message  = message;

    std::vector<AlertChannel> targets;
    for (const auto& ch : channels_) {
        if (channel_complete(ch)) targets.push_back(ch);
    }
    if (targets.empty()) return 0;
    const int queued = static_cast<int>(targets.size());

    // The job owns copies of everything it needs; the tick moves on.
    const bool ok = worker_.post(
        [this, chs = std::move(targets), sev, rule_name, message]
        (const std::atomic<bool>& cancel) {
            for (const auto& ch : chs) {
                if (cancel.load()) {               // shutting down
                    failed_.fetch_add(1);
                    continue;
                }
                std::string why;
                if (send_one(ch, sev, rule_name, message, &cancel, &why)) {
                    delivered_.fetch_add(1);
                } else {
                    failed_.fetch_add(1);
                    std::fprintf(stderr,
                        "budyk alert: channel '%s' (%s) failed: %s\n",
                        ch.name.c_str(), ch.type.c_str(), why.c_str());
                }
            }
        });
    if (!ok) {
        std::fprintf(stderr, "budyk alert: queue full, alert for rule '%s' dropped\n",
                     rule_name.c_str());
        return 0;
    }
    return queued;
}

} // namespace budyk
