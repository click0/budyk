// SPDX-License-Identifier: BSD-3-Clause
#include "tui/tui.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
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
// One-shot GET against host:port/path. Closes the connection after the
// peer EOFs. Returns 0 on success and the response body in `body`,
// negative on connect / send / parse failure.
int http_get(const char* host, int port, const char* path, std::string* body) {
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

    char req[512];
    int n = std::snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
        path, host, port);
    if (n <= 0 || ::send(fd, req, static_cast<size_t>(n), 0) != n) {
        ::close(fd);
        return -4;
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

    auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) return -6;
    body->assign(raw, sep + 4, std::string::npos);
    return 0;
}

// --- minimal JSON probes -----------------------------------------------------
// Find the LAST occurrence of `"<key>":` and parse the following number.
// Suitable for our /api/samples response — the most-recent sample's
// values land last by virtue of the array order. Returns the fallback
// when the key is absent or the value is malformed.
double last_number(const std::string& s, const char* key, double fallback = 0.0) {
    std::string needle = "\"";
    needle.append(key);
    needle.append("\":");
    auto pos = s.rfind(needle);
    if (pos == std::string::npos) return fallback;
    pos += needle.size();
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    char* endp = nullptr;
    double v = std::strtod(s.c_str() + pos, &endp);
    if (endp == s.c_str() + pos) return fallback;
    return v;
}

uint64_t last_uint(const std::string& s, const char* key) {
    double v = last_number(s, key, 0.0);
    return v < 0.0 ? 0 : static_cast<uint64_t>(v);
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
    int filled = static_cast<int>((pct / 100.0) * cells + 0.5);
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

int tui_run(const char* host, int port) {
#ifndef BUDYK_HAVE_CURSES
    (void)host; (void)port;
    std::fprintf(stderr,
        "budyk tui: built without ncurses — install ncurses-dev and rebuild.\n");
    return -1;
#else
    if (host == nullptr || *host == '\0') host = "127.0.0.1";
    if (port <= 0)                        port = 8080;

    // --- ncurses init --------------------------------------------------------
    ::initscr();
    ::cbreak();
    ::noecho();
    ::curs_set(0);
    ::keypad(stdscr, TRUE);
    ::wtimeout(stdscr, 1000);   // getch() blocks at most 1 s

    bool quit = false;
    int  tick = 0;
    std::string err;

    while (!quit) {
        std::string body;
        const int rc = http_get(host, port, "/api/samples", &body);

        ::erase();
        ::mvprintw(0, 0, "budyk tui  %s:%d   q=quit", host, port);
        ::mvprintw(0, COLS - 12, "tick #%d", tick);

        if (rc != 0) {
            ::mvprintw(2, 0, "connection error (rc=%d) — retrying in 1s", rc);
            err = "connection error";
        } else {
            // If the server returned a 401 / 4xx the body still parses
            // — last_number simply returns 0.0 and the user sees an
            // empty dashboard, which is the same UX as "no data".
            const double cpu_pct  = last_number(body, "total_percent");
            const uint32_t cores  = static_cast<uint32_t>(last_uint(body, "count"));
            const uint64_t mt     = last_uint(body, "total");           // memory total
            const uint64_t ma     = last_uint(body, "available");
            const double  mem_av  = last_number(body, "available_percent");
            const double  swap_us = last_number(body, "used_percent");
            const double  load1   = last_number(body, "avg_1m");
            const double  load5   = last_number(body, "avg_5m");
            const double  load15  = last_number(body, "avg_15m");
            const uint64_t dr     = last_uint(body, "read_bytes_per_sec");
            const uint64_t dw     = last_uint(body, "write_bytes_per_sec");
            const uint32_t devs   = static_cast<uint32_t>(last_uint(body, "device_count"));
            const uint64_t rx     = last_uint(body, "rx_bytes_per_sec");
            const uint64_t tx     = last_uint(body, "tx_bytes_per_sec");
            const uint32_t ifs    = static_cast<uint32_t>(last_uint(body, "interface_count"));
            const double  uptime  = last_number(body, "uptime_seconds");

            const int barw = COLS > 50 ? COLS - 30 : 20;
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
