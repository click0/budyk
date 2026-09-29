// SPDX-License-Identifier: BSD-3-Clause
#include "tui/tui.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <strings.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef BUDYK_HAVE_CURSES
#  include <curses.h>
#endif

namespace budyk {

namespace {

// --- minimal HTTP client -----------------------------------------------------
// One request per connection against host:port. `extra_headers` is
// appended verbatim (each line ending in \r\n). Fills *reply and returns 0,
// or a negative code on connect / send / parse failure.
int http_request(const char* host, int port, const char* method,
                 const char* path, const std::string& extra_headers,
                 const std::string& body, tui_detail::HttpReply* reply) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host, &sa.sin_addr) != 1) { ::close(fd); return -2; }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        ::close(fd);
        return -3;
    }

    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + std::string(host) + ":" + std::to_string(port) + "\r\n";
    req += "Connection: close\r\n";
    req += extra_headers;
    if (!body.empty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; ::close(fd); return -4; }
        sent += static_cast<size_t>(n);
    }

    std::string raw;
    char buf[4096];
    while (true) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r < 0) { if (errno == EINTR) continue; ::close(fd); return -5; }
        if (r == 0) break;
        raw.append(buf, static_cast<size_t>(r));
    }
    ::close(fd);
    return tui_detail::parse_http_reply(raw, reply) ? 0 : -6;
}

// POST /api/auth/login; on success *cookie becomes "budyk_session=<token>".
bool login(const char* host, int port, const std::string& password,
           std::string* cookie) {
    tui_detail::HttpReply r;
    const std::string body = "{\"password\":" + tui_detail::json_quote(password) + "}";
    if (http_request(host, port, "POST", "/api/auth/login", "", body, &r) != 0) {
        return false;
    }
    if (r.status != 200) return false;
    *cookie = tui_detail::session_cookie(r.headers);
    return !cookie->empty();
}

// --- formatting helpers ------------------------------------------------------
std::string fmt_bytes(uint64_t b) {
    char buf[32];
    if      (b >= (1ULL << 40)) std::snprintf(buf, sizeof(buf), "%.1fT", b / 1099511627776.0);
    else if (b >= (1ULL << 30)) std::snprintf(buf, sizeof(buf), "%.1fG", b / 1073741824.0);
    else if (b >= (1ULL << 20)) std::snprintf(buf, sizeof(buf), "%.1fM", b / 1048576.0);
    else if (b >= (1ULL << 10)) std::snprintf(buf, sizeof(buf), "%.1fK", b / 1024.0);
    else                        std::snprintf(buf, sizeof(buf), "%lluB", static_cast<unsigned long long>(b));
    return buf;
}

std::string fmt_uptime(double s) {
    if (s < 0) s = 0;
    const uint64_t total = static_cast<uint64_t>(s);
    const uint64_t d = total / 86400;
    const uint64_t h = (total % 86400) / 3600;
    const uint64_t m = (total % 3600) / 60;
    const uint64_t sec = total % 60;
    char buf[64];
    if (d > 0)      std::snprintf(buf, sizeof(buf), "%llud %lluh %llum",
                                  (unsigned long long)d, (unsigned long long)h, (unsigned long long)m);
    else if (h > 0) std::snprintf(buf, sizeof(buf), "%lluh %llum %llus",
                                  (unsigned long long)h, (unsigned long long)m, (unsigned long long)sec);
    else            std::snprintf(buf, sizeof(buf), "%llum %llus",
                                  (unsigned long long)m, (unsigned long long)sec);
    return buf;
}

// `pct` clamped to [0, 100]; produces "[#####.....]" of width `cells`.
std::string bar(double pct, int cells) {
    if (pct < 0)        pct = 0;
    if (pct > 100)      pct = 100;
    if (cells < 2)      cells = 2;
    int filled = static_cast<int>(std::lround((pct / 100.0) * cells));
    if (filled > cells) filled = cells;
    std::string out;
    out.reserve(static_cast<size_t>(cells) + 2);
    out.push_back('[');
    out.append(static_cast<size_t>(filled),         '#');
    out.append(static_cast<size_t>(cells - filled), '.');
    out.push_back(']');
    return out;
}

} // namespace

namespace tui_detail {

bool parse_http_reply(const std::string& raw, HttpReply* out) {
    if (out == nullptr) return false;
    const auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) return false;
    // "HTTP/1.1 200 OK"
    const auto sp = raw.find(' ');
    if (raw.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos || sp > sep) {
        return false;
    }
    char* end = nullptr;
    const long code = std::strtol(raw.c_str() + sp + 1, &end, 10);
    if (end == raw.c_str() + sp + 1 || code < 100 || code > 599) return false;
    out->status = static_cast<int>(code);
    const auto eol = raw.find("\r\n");
    out->headers.assign(raw, eol + 2, sep > eol ? sep - eol - 2 : 0);
    out->body.assign(raw, sep + 4, std::string::npos);
    return true;
}

std::string session_cookie(const std::string& headers) {
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t eol = headers.find("\r\n", pos);
        if (eol == std::string::npos) eol = headers.size();
        const std::string line = headers.substr(pos, eol - pos);
        pos = eol + 2;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (::strncasecmp(line.c_str(), "Set-Cookie", colon) != 0 || colon != 10) continue;
        auto v = line.find_first_not_of(' ', colon + 1);
        if (v == std::string::npos) continue;
        const auto semi = line.find(';', v);
        const std::string pair = line.substr(v, semi == std::string::npos ? std::string::npos : semi - v);
        if (pair.compare(0, 14, "budyk_session=") == 0 && pair.size() > 14) return pair;
    }
    return std::string();
}

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out += "\"";
    return out;
}

double sample_number(const std::string& body, const char* section,
                     const char* key, double fallback) {
    // The newest sample is the last one in the array; its fields come
    // after every earlier sample's, so search from its start.
    size_t from = body.rfind("{\"ts\":");
    if (from == std::string::npos) from = 0;
    size_t end = body.size();
    if (section != nullptr) {
        const std::string open = "\"" + std::string(section) + "\":{";
        const auto at = body.find(open, from);
        if (at == std::string::npos) return fallback;
        from = at + open.size();
        end  = body.find('}', from);        // sections are flat objects
        if (end == std::string::npos) return fallback;
    }
    const std::string needle = "\"" + std::string(key) + "\":";
    auto pos = body.find(needle, from);
    if (pos == std::string::npos || pos >= end) return fallback;
    pos += needle.size();
    while (pos < end && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    char* endp = nullptr;
    const double v = std::strtod(body.c_str() + pos, &endp);
    if (endp == body.c_str() + pos) return fallback;
    return v;
}

} // namespace tui_detail

int tui_run(const char* host, int port, std::string (*ask_password)()) {
#ifndef BUDYK_HAVE_CURSES
    (void)host; (void)port; (void)ask_password;
    std::fprintf(stderr,
        "budyk tui: built without ncurses — install ncurses-dev and rebuild.\n");
    return -1;
#else
    if (host == nullptr || *host == '\0') host = "127.0.0.1";
    if (port <= 0)                        port = 8080;

    // Log in before taking over the screen, if the daemon asks for it.
    // The password stays in memory so an expired session (24 h, or a
    // daemon restart without persist_sessions) can be renewed silently.
    std::string password;
    std::string cookie_header;   // "Cookie: budyk_session=...\r\n" or ""
    {
        tui_detail::HttpReply probe;
        const int rc = http_request(host, port, "GET", "/api/samples", "", "", &probe);
        if (rc != 0) {
            std::fprintf(stderr, "budyk tui: can't reach %s:%d (rc=%d)\n", host, port, rc);
            return -1;
        }
        if (probe.status == 401) {
            if (ask_password == nullptr) {
                std::fprintf(stderr, "budyk tui: %s:%d requires a login\n", host, port);
                return -1;
            }
            password = ask_password();
            std::string cookie;
            if (!login(host, port, password, &cookie)) {
                std::fprintf(stderr, "budyk tui: login to %s:%d failed (wrong password?)\n",
                             host, port);
                return -1;
            }
            cookie_header = "Cookie: " + cookie + "\r\n";
        }
    }

    // --- ncurses init --------------------------------------------------------
    ::initscr();
    ::cbreak();
    ::noecho();
    ::curs_set(0);
    ::keypad(stdscr, TRUE);
    ::wtimeout(stdscr, 1000);   // getch() blocks at most 1 s

    bool quit = false;
    int  tick = 0;

    while (!quit) {
        tui_detail::HttpReply r;
        int rc = http_request(host, port, "GET", "/api/samples", cookie_header, "", &r);
        if (rc == 0 && r.status == 401 && !password.empty()) {
            std::string cookie;                       // session expired: renew
            if (login(host, port, password, &cookie)) {
                cookie_header = "Cookie: " + cookie + "\r\n";
                rc = http_request(host, port, "GET", "/api/samples", cookie_header, "", &r);
            }
        }

        ::erase();
        ::mvprintw(0, 0, "budyk tui  %s:%d   q=quit", host, port);
        ::mvprintw(0, COLS - 12, "tick #%d", tick);

        if (rc != 0) {
            ::mvprintw(2, 0, "connection error (rc=%d) — retrying in 1s", rc);
        } else if (r.status != 200) {
            // Say what the server said instead of drawing zeros.
            ::mvprintw(2, 0, "server returned HTTP %d%s — retrying in 1s", r.status,
                       r.status == 401 ? " (login required or expired)" : "");
        } else {
            using tui_detail::sample_number;
            const std::string& b = r.body;
            const double   cpu_pct = sample_number(b, "cpu", "total_percent");
            const uint32_t cores   = static_cast<uint32_t>(sample_number(b, "cpu", "count"));
            const uint64_t mt      = static_cast<uint64_t>(sample_number(b, "mem", "total"));
            const uint64_t ma      = static_cast<uint64_t>(sample_number(b, "mem", "available"));
            const double   mem_av  = sample_number(b, "mem", "available_percent");
            const double   swap_us = sample_number(b, "swap", "used_percent");
            const double   load1   = sample_number(b, "load", "avg_1m");
            const double   load5   = sample_number(b, "load", "avg_5m");
            const double   load15  = sample_number(b, "load", "avg_15m");
            const uint64_t dr      = static_cast<uint64_t>(sample_number(b, "disk", "read_bytes_per_sec"));
            const uint64_t dw      = static_cast<uint64_t>(sample_number(b, "disk", "write_bytes_per_sec"));
            const uint32_t devs    = static_cast<uint32_t>(sample_number(b, "disk", "device_count"));
            const uint64_t rx      = static_cast<uint64_t>(sample_number(b, "net", "rx_bytes_per_sec"));
            const uint64_t tx      = static_cast<uint64_t>(sample_number(b, "net", "tx_bytes_per_sec"));
            const uint32_t ifs     = static_cast<uint32_t>(sample_number(b, "net", "interface_count"));
            const double   uptime  = sample_number(b, nullptr, "uptime_seconds");

            // Leave room for the longest line: "Memory 100.0% [bar] free
            // 1023.9G / 1023.9G" is 40 columns plus the bar. COLS - 30 used
            // to push the memory total off the right edge at every width.
            const int barw = COLS > 50 ? COLS - 40 : 10;
            ::mvprintw(2, 0, "CPU    %5.1f%% %s %u cores",
                       cpu_pct, bar(cpu_pct, barw).c_str(), cores);
            ::mvprintw(3, 0, "Memory %5.1f%% %s free %s / %s",
                       100.0 - mem_av, bar(100.0 - mem_av, barw).c_str(),
                       fmt_bytes(ma).c_str(), fmt_bytes(mt).c_str());
            ::mvprintw(4, 0, "Swap   %5.1f%% %s",
                       swap_us, bar(swap_us, barw).c_str());
            ::mvprintw(6, 0, "Load   %.2f / %.2f / %.2f", load1, load5, load15);
            ::mvprintw(7, 0, "Disk   r %s/s   w %s/s   (%u devs)",
                       fmt_bytes(dr).c_str(), fmt_bytes(dw).c_str(), devs);
            ::mvprintw(8, 0, "Net    rx %s/s  tx %s/s   (%u ifaces)",
                       fmt_bytes(rx).c_str(), fmt_bytes(tx).c_str(), ifs);
            ::mvprintw(9, 0, "Uptime %s", fmt_uptime(uptime).c_str());
        }
        ::refresh();

        // getch() blocks up to 1 s thanks to wtimeout(), then returns
        // ERR on no-input or the keycode otherwise.
        int ch = ::getch();
        if (ch == 'q' || ch == 'Q' || ch == 27 /*ESC*/) quit = true;
        ++tick;
    }

    ::endwin();
    return 0;
#endif
}

} // namespace budyk
