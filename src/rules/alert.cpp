// SPDX-License-Identifier: BSD-3-Clause
#include "rules/alert.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
