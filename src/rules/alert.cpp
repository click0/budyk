// SPDX-License-Identifier: BSD-3-Clause
#include "rules/alert.h"
#include "rules/exec_action.h"
#include "core/json_text.h"
#include "util/tmpfile.h"

#include <strings.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
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
    for (unsigned char c : s) {
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

// Temp files for curl: bodies, header files, the -K url config and
// netrc. Private (0600) and unlinked once curl exits.
bool write_tmp(const std::string& body, char* path_out, size_t cap) {
    return write_private_tmp("budyk_alert_", body, path_out, cap);
}

// "machine <host> login <user> password <pass>\n" for --netrc-file,
// with the host taken from the URL. False when the URL has no host or
// user_pass has no ':'.
bool netrc_for(const char* url, const std::string& user_pass, std::string* out) {
    const char* p = std::strstr(url, "://");
    if (p == nullptr) return false;
    p += 3;
    const char* end = p;
    while (*end != '\0' && *end != '/' && *end != ':') ++end;
    const auto colon = user_pass.find(':');
    if (colon == std::string::npos) return false;
    *out  = "machine ";
    out->append(p, end);
    *out += " login ";
    *out += user_pass.substr(0, colon);
    *out += " password ";
    *out += user_pass.substr(colon + 1);
    *out += "\n";
    return true;
}

// A curl config file (-K) holding the URL, so it never appears in
// curl's argv: a Telegram bot token sits in the URL path and a Discord
// webhook URL is itself the credential, and argv is readable by every
// local user through ps(1). The file is 0600 from mkstemp and unlinked
// as soon as curl exits. Quoting per curl's config syntax: backslash,
// double quote and the control characters that would start a new line
// or option are escaped, so a URL from the config file is one `url`
// value whatever it contains.
bool write_url_config(const char* url, char* path_out, size_t cap) {
    std::string cfg = "url = \"";
    for (const char* p = url; *p != '\0'; ++p) {
        switch (*p) {
            case '\\': cfg += "\\\\"; break;
            case '"':  cfg += "\\\""; break;
            case '\n': cfg += "\\n";  break;
            case '\r': cfg += "\\r";  break;
            case '\t': cfg += "\\t";  break;
            default:   cfg += *p;     break;
        }
    }
    cfg += "\"\n";
    return write_tmp(cfg, path_out, cap);
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
// through a -K config file (write_url_config), so it is neither an
// option nor visible in ps(1). stdio is
// /dev/null and the run is killed after timeout_s (see exec_command).
// Returns 0 when curl exits 0.
int run_curl(const std::vector<std::string>& args, int timeout_s,
             const std::atomic<bool>* cancel) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back("curl");
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    ExecResult res{};
    if (exec_command(argv.data(), timeout_s, &res, cancel) != 0) return -1;
    return res.exit_status == 0 && res.signal == 0 && !res.timed_out ? 0 : -2;
}

// Shared scaffold for the curl invocations below: writes body+headers
// to /tmp, runs curl with --max-time 10, returns rc==0 on HTTP success
// (curl already maps 4xx/5xx to non-zero via -f… but we don't use -f
// because the dispatcher is best-effort and we don't want curl to
// treat a 4xx as fatal-to-the-batch).
int curl_post(const char* url,
              const std::string& body,
              const std::vector<std::string>& extra_headers,
              const std::atomic<bool>* cancel) {
    char body_path[64];
    if (!write_tmp(body, body_path, sizeof(body_path))) return -1;

    std::string headers;
    for (const auto& h : extra_headers) {
        headers += h;
        headers += "\n";
    }
    char hdr_path[64];
    if (!write_tmp(headers, hdr_path, sizeof(hdr_path))) {
        ::unlink(body_path);
        return -2;
    }
    char url_path[64];
    if (!write_url_config(url, url_path, sizeof(url_path))) {
        ::unlink(body_path);
        ::unlink(hdr_path);
        return -2;
    }

    const int rc = run_curl({"-sS", "-X", "POST",
                             "-H", std::string("@") + hdr_path,
                             "-d", std::string("@") + body_path,
                             "--max-time", "10", "-K", url_path}, 15, cancel);
    ::unlink(body_path);
    ::unlink(hdr_path);
    ::unlink(url_path);
    return rc == 0 ? 0 : -3;
}

// HTTP POST with HTTP basic auth and an x-www-form-urlencoded body.
// Used for Twilio's REST API; basic-auth credentials ride via curl's
// --netrc-file so they don't appear in `ps`/argv.
int curl_basic_form_post(const char* url,
                         const std::string& user_pass,
                         const std::string& form_body,
              const std::atomic<bool>* cancel) {
    char body_path[64];
    if (!write_tmp(form_body, body_path, sizeof(body_path))) return -1;

    // netrc: curl --netrc-file matches by host, taken from the URL.
    std::string netrc_blob;
    if (!netrc_for(url, user_pass, &netrc_blob)) { ::unlink(body_path); return -2; }
    char netrc_path[64];
    if (!write_tmp(netrc_blob, netrc_path, sizeof(netrc_path))) {
        ::unlink(body_path);
        return -3;
    }
    char url_path[64];
    if (!write_url_config(url, url_path, sizeof(url_path))) {
        ::unlink(body_path);
        ::unlink(netrc_path);
        return -3;
    }

    const int rc = run_curl({"-sS", "-X", "POST", "--netrc-file", netrc_path,
                             "-H", "Content-Type: application/x-www-form-urlencoded",
                             "-d", std::string("@") + body_path,
                             "--max-time", "10", "-K", url_path}, 15, cancel);
    ::unlink(body_path);
    ::unlink(netrc_path);
    ::unlink(url_path);
    return rc == 0 ? 0 : -4;
}

// SMTP delivery via curl. Credentials again pass through --netrc-file
// (so neither the SMTP user nor password is visible in argv).
//   url: smtp://host:port or smtps://host:port
//   user_pass: "user:pass" (may be empty for unauth relays)
//   from/to: envelope sender + recipient
//   message: full RFC822 DATA blob (headers + blank line + body)
int curl_smtp(const char* url,
              const std::string& user_pass,
              const std::string& from,
              const std::string& to,
              const std::string& message,
              const std::atomic<bool>* cancel) {
    char body_path[64];
    if (!write_tmp(message, body_path, sizeof(body_path))) return -1;

    char netrc_path[64] = {0};
    if (!user_pass.empty()) {
        std::string netrc_blob;
        if (!netrc_for(url, user_pass, &netrc_blob)) { ::unlink(body_path); return -2; }
        if (!write_tmp(netrc_blob, netrc_path, sizeof(netrc_path))) {
            ::unlink(body_path);
            return -4;
        }
    }

    std::vector<std::string> args = {"-sS"};
    if (netrc_path[0] != '\0') {
        args.push_back("--netrc-file");
        args.push_back(netrc_path);
    }
    char url_path[64];
    if (!write_url_config(url, url_path, sizeof(url_path))) {
        ::unlink(body_path);
        if (netrc_path[0] != '\0') ::unlink(netrc_path);
        return -4;
    }
    args.insert(args.end(), {"--mail-from", from, "--mail-rcpt", to,
                             "-T", body_path, "--max-time", "15", "-K", url_path});
    const int rc = run_curl(args, 20, cancel);
    ::unlink(body_path);
    ::unlink(url_path);
    if (netrc_path[0] != '\0') ::unlink(netrc_path);
    return rc == 0 ? 0 : -5;
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
    out += "From: ";    out += from; out += "\r\n";
    out += "To: ";      out += to;   out += "\r\n";
    out += "Subject: [budyk:"; out += severity_name(sev); out += "] "; out += rule_name; out += "\r\n";
    out += "Date: ";    out += date_buf; out += "\r\n";
    out += "MIME-Version: 1.0\r\n";
    out += "Content-Type: text/plain; charset=utf-8\r\n";
    out += "\r\n";
    out += message;
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

// One channel, one alert. Runs on the worker thread. 0 when curl
// exited 0.
int send_one(const AlertChannel& ch, AlertSeverity sev,
             const std::string& rule_name, const std::string& message,
             const std::atomic<bool>* cancel) {
    if (ch.type == "ntfy") {
        // ntfy.sh: POST to <base>/<topic>, body = message,
        // headers: Title, Priority, Tags.
        std::string url = ch.url;
        if (!url.empty() && url.back() != '/') url.push_back('/');
        url += ch.topic;
        std::vector<std::string> hdrs = {
            std::string("Title: budyk: ") + rule_name,
            std::string("Priority: ") + ntfy_priority(sev),
            std::string("Tags: ") + severity_name(sev),
        };
        return curl_post(url.c_str(), ntfy_payload(sev, rule_name, message), hdrs, cancel);
    }
    if (ch.type == "discord") {
        std::vector<std::string> hdrs = {
            "Content-Type: application/json",
        };
        return curl_post(ch.url.c_str(),
                         discord_payload(sev, rule_name, message), hdrs, cancel);
    }
    if (ch.type == "telegram") {
        // url override; default to the public Bot API endpoint.
        std::string url = ch.url;
        if (url.empty()) {
            url  = "https://api.telegram.org/bot";
            url += ch.token;
            url += "/sendMessage";
        }
        std::vector<std::string> hdrs = {
            "Content-Type: application/json",
        };
        return curl_post(url.c_str(),
                         telegram_payload(sev, ch.topic, rule_name, message),
                         hdrs, cancel);
    }
    if (ch.type == "smtp") {
        return curl_smtp(ch.url.c_str(), ch.token,
                         ch.from, ch.topic,
                         smtp_message(sev, ch.from, ch.topic,
                                      rule_name, message), cancel);
    }
    if (ch.type == "twilio") {
        return curl_basic_form_post(ch.url.c_str(), ch.token,
                                    twilio_form(ch.from, ch.topic,
                                                rule_name, message), cancel);
    }
    return -4;
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
                const int rc = send_one(ch, sev, rule_name, message, &cancel);
                if (rc == 0) {
                    delivered_.fetch_add(1);
                } else {
                    failed_.fetch_add(1);
                    std::fprintf(stderr,
                        "budyk alert: channel '%s' (%s) failed (rc=%d)\n",
                        ch.name.c_str(), ch.type.c_str(), rc);
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
