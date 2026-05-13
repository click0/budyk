// SPDX-License-Identifier: BSD-3-Clause
#include "rules/alert.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

namespace budyk {

namespace {

// JSON-escape helper. Same minimal implementation as ai/llm_client —
// duplicated here so alert isn't pulled into budyk_ai's dep graph.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

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

bool write_tmp(const std::string& body, char* path_out, size_t cap) {
    if (cap < 32) return false;
    std::strcpy(path_out, "/tmp/budyk_alert_XXXXXX");
    int fd = ::mkstemp(path_out);
    if (fd < 0) return false;
    const ssize_t n = ::write(fd, body.data(), body.size());
    ::close(fd);
    return n == static_cast<ssize_t>(body.size());
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

// Shared scaffold for the curl invocations below: writes body+headers
// to /tmp, runs curl with --max-time 10, returns rc==0 on HTTP success
// (curl already maps 4xx/5xx to non-zero via -f… but we don't use -f
// because the dispatcher is best-effort and we don't want curl to
// treat a 4xx as fatal-to-the-batch).
int curl_post(const char* url,
              const std::string& body,
              const std::vector<std::string>& extra_headers) {
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

    char cmd[2048];
    std::snprintf(cmd, sizeof(cmd),
        "curl -sS -X POST -H @%s -d @%s --max-time 10 %s >/dev/null 2>&1",
        hdr_path, body_path, url);

    const int rc = std::system(cmd);
    ::unlink(body_path);
    ::unlink(hdr_path);
    return rc == 0 ? 0 : -3;
}

// HTTP POST with HTTP basic auth and an x-www-form-urlencoded body.
// Used for Twilio's REST API; basic-auth credentials ride via curl's
// --netrc-file so they don't appear in `ps`/argv.
int curl_basic_form_post(const char* url,
                         const std::string& user_pass,
                         const std::string& form_body) {
    char body_path[64];
    if (!write_tmp(form_body, body_path, sizeof(body_path))) return -1;

    // netrc: "machine <host> login <u> password <p>" — curl --netrc-file
    // matches by host, so we extract it from the URL.
    std::string host;
    {
        const char* p = std::strstr(url, "://");
        if (p == nullptr) { ::unlink(body_path); return -2; }
        p += 3;
        const char* end = p;
        while (*end != '\0' && *end != '/' && *end != ':') ++end;
        host.assign(p, end);
    }
    const auto colon = user_pass.find(':');
    if (colon == std::string::npos) { ::unlink(body_path); return -2; }
    std::string netrc_blob = "machine ";
    netrc_blob += host;
    netrc_blob += " login ";
    netrc_blob += user_pass.substr(0, colon);
    netrc_blob += " password ";
    netrc_blob += user_pass.substr(colon + 1);
    netrc_blob += "\n";
    char netrc_path[64];
    if (!write_tmp(netrc_blob, netrc_path, sizeof(netrc_path))) {
        ::unlink(body_path);
        return -3;
    }
    ::chmod(netrc_path, 0600);

    char cmd[2048];
    std::snprintf(cmd, sizeof(cmd),
        "curl -sS -X POST --netrc-file %s "
        "-H 'Content-Type: application/x-www-form-urlencoded' "
        "-d @%s --max-time 10 %s >/dev/null 2>&1",
        netrc_path, body_path, url);

    const int rc = std::system(cmd);
    ::unlink(body_path);
    ::unlink(netrc_path);
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
              const std::string& message) {
    char body_path[64];
    if (!write_tmp(message, body_path, sizeof(body_path))) return -1;

    std::string auth_args;
    char netrc_path[64] = {0};
    if (!user_pass.empty()) {
        const auto colon = user_pass.find(':');
        if (colon == std::string::npos) { ::unlink(body_path); return -2; }
        // Extract host from URL (smtp://host:port or smtps://host:port)
        std::string host;
        const char* p = std::strstr(url, "://");
        if (p == nullptr) { ::unlink(body_path); return -3; }
        p += 3;
        const char* end = p;
        while (*end != '\0' && *end != '/' && *end != ':') ++end;
        host.assign(p, end);
        std::string netrc_blob = "machine ";
        netrc_blob += host;
        netrc_blob += " login ";
        netrc_blob += user_pass.substr(0, colon);
        netrc_blob += " password ";
        netrc_blob += user_pass.substr(colon + 1);
        netrc_blob += "\n";
        if (!write_tmp(netrc_blob, netrc_path, sizeof(netrc_path))) {
            ::unlink(body_path);
            return -4;
        }
        ::chmod(netrc_path, 0600);
        auth_args = "--netrc-file ";
        auth_args += netrc_path;
        auth_args += " ";
    }

    char cmd[2048];
    std::snprintf(cmd, sizeof(cmd),
        "curl -sS %s--mail-from '%s' --mail-rcpt '%s' "
        "-T %s --max-time 15 %s >/dev/null 2>&1",
        auth_args.c_str(), from.c_str(), to.c_str(), body_path, url);

    const int rc = std::system(cmd);
    ::unlink(body_path);
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

void AlertDispatcher::add_channel(AlertChannel ch) {
    channels_.emplace_back(std::move(ch));
}

size_t AlertDispatcher::channel_count() const { return channels_.size(); }

int AlertDispatcher::dispatch(AlertSeverity sev,
                              const std::string& rule_name,
                              const std::string& message) {
    int succeeded = 0;
    for (const auto& ch : channels_) {
        int rc = -1;
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
            rc = curl_post(url.c_str(), ntfy_payload(sev, rule_name, message), hdrs);
        } else if (ch.type == "discord") {
            std::vector<std::string> hdrs = {
                "Content-Type: application/json",
            };
            rc = curl_post(ch.url.c_str(),
                           discord_payload(sev, rule_name, message), hdrs);
        } else if (ch.type == "telegram") {
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
            rc = curl_post(url.c_str(),
                           telegram_payload(sev, ch.topic, rule_name, message),
                           hdrs);
        } else if (ch.type == "smtp") {
            if (ch.from.empty() || ch.topic.empty() || ch.url.empty()) {
                std::fprintf(stderr,
                    "budyk alert: smtp channel '%s' missing url/from/topic\n",
                    ch.name.c_str());
                continue;
            }
            rc = curl_smtp(ch.url.c_str(), ch.token,
                           ch.from, ch.topic,
                           smtp_message(sev, ch.from, ch.topic,
                                        rule_name, message));
        } else if (ch.type == "twilio") {
            if (ch.from.empty() || ch.topic.empty() || ch.url.empty() ||
                ch.token.empty()) {
                std::fprintf(stderr,
                    "budyk alert: twilio channel '%s' missing url/token/from/topic\n",
                    ch.name.c_str());
                continue;
            }
            rc = curl_basic_form_post(ch.url.c_str(), ch.token,
                                      twilio_form(ch.from, ch.topic,
                                                  rule_name, message));
        } else {
            std::fprintf(stderr,
                "budyk alert: unknown channel type '%s' on '%s'\n",
                ch.type.c_str(), ch.name.c_str());
            continue;
        }
        if (rc == 0) ++succeeded;
        else        std::fprintf(stderr,
            "budyk alert: channel '%s' (%s) failed (rc=%d)\n",
            ch.name.c_str(), ch.type.c_str(), rc);
    }
    return succeeded;
}

} // namespace budyk
