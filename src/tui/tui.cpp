// SPDX-License-Identifier: BSD-3-Clause
#include "tui/tui.h"

#include "core/json_text.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#ifdef BUDYK_HAVE_CURSES
#  include <curses.h>
#endif

namespace budyk {

namespace {

// --- minimal HTTP client -----------------------------------------------------
// One request per connection against host:port. `extra_headers` is
// appended verbatim (each line ending in \r\n). Fills *reply and returns 0,
// or a negative code on connect / send / parse failure.
// errno of the last failed syscall in http_request (0 when the failure
// was not a syscall). The TUI is single-threaded.
int g_http_errno = 0;

// http_request's return code in words, with the errno where there is
// one: "connect: Connection refused".
std::string http_error(int rc) {
    const char* what = "unknown error";
    switch (rc) {
        case -1: what = "socket";               break;
        case -2: what = "not an IPv4 address";  break;
        case -3: what = "connect";              break;
        case -4: what = "send";                 break;
        case -5: what = "recv";                 break;
        case -6: what = "malformed HTTP reply"; break;
        default: break;
    }
    std::string out = what;
    if (g_http_errno != 0) {
        out += ": ";
        out += std::strerror(g_http_errno);
    }
    return out;
}

// A request is bounded: a daemon stopped (SIGSTOP), a host that does not
// answer, a network cut held the TUI in connect() or recv(), `q` unread.
// The bound is on a wait with no progress, not on the whole request: a
// big reply over a slow link takes as long as it takes. It is longer than
// the daemon's own bound on a connection (5 s, HttpServer's I/O timeout),
// since its one HTTP thread may give that whole to an idle client queued
// ahead of the TUI (a browser's preconnect, a probe).
constexpr int64_t kHttpIdleMs = 8000;

int64_t monotonic_ms() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Until `fd` is ready for `events` or the deadline (monotonic_ms())
// passes: true if ready, else false with errno ETIMEDOUT.
bool wait_ready(int fd, short events, int64_t deadline_ms) {
    while (true) {
        const int64_t left = deadline_ms - monotonic_ms();
        if (left <= 0) { errno = ETIMEDOUT; return false; }
        struct pollfd p{fd, events, 0};
        const int r = ::poll(&p, 1, static_cast<int>(left));
        if (r > 0) return true;
        if (r < 0 && errno != EINTR) return false;
    }
}

int http_request(const char* host, int port, const char* method,
                 const char* path, const std::string& extra_headers,
                 const std::string& body, tui_detail::HttpReply* reply) {
    g_http_errno = 0;
    int64_t deadline = monotonic_ms() + kHttpIdleMs;   // moved on by progress
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { g_http_errno = errno; return -1; }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host, &sa.sin_addr) != 1) { ::close(fd); return -2; }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        int err = errno;
        if (err == EINPROGRESS && wait_ready(fd, POLLOUT, deadline)) {
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;
        } else if (err == EINPROGRESS) {
            err = errno;
        }
        if (err != 0) {
            g_http_errno = err;
            ::close(fd);
            return -3;
        }
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
        if (n >= 0) {
            sent += static_cast<size_t>(n);
            deadline = monotonic_ms() + kHttpIdleMs;
            continue;
        }
        if (errno == EINTR) continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && wait_ready(fd, POLLOUT, deadline)) continue;
        g_http_errno = errno;
        ::close(fd);
        return -4;
    }

    std::string raw;
    char buf[4096];
    while (true) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r == 0) break;
        if (r > 0) {
            raw.append(buf, static_cast<size_t>(r));
            deadline = monotonic_ms() + kHttpIdleMs;
            continue;
        }
        if (errno == EINTR) continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && wait_ready(fd, POLLIN, deadline)) continue;
        g_http_errno = errno;
        ::close(fd);
        return -5;
    }
    ::close(fd);
    return tui_detail::parse_http_reply(raw, reply) ? 0 : -6;
}

// POST /api/auth/login; on success *cookie becomes "budyk_session=<token>".
// Returns the HTTP status (200 = logged in; 401 wrong password; 429
// throttled after too many failures), 0 for a 200 with no cookie, or
// http_request's negative code when the daemon can't be reached (a
// timeout was reported as a wrong password).
int login(const char* host, int port, const std::string& password,
          std::string* cookie) {
    tui_detail::HttpReply r;
    const std::string body = "{\"password\":" + tui_detail::json_quote(password) + "}";
    const int rc = http_request(host, port, "POST", "/api/auth/login", "", body, &r);
    if (rc != 0) return rc;
    if (r.status != 200) return r.status;
    *cookie = tui_detail::session_cookie(r.headers);
    return cookie->empty() ? 0 : 200;
}

// --- formatting helpers ------------------------------------------------------
std::string fmt_bytes(uint64_t b) {
    char buf[32];
    if      (b >= (1ULL << 40)) std::snprintf(buf, sizeof(buf), "%.1fT", static_cast<double>(b) / 1099511627776.0);
    else if (b >= (1ULL << 30)) std::snprintf(buf, sizeof(buf), "%.1fG", static_cast<double>(b) / 1073741824.0);
    else if (b >= (1ULL << 20)) std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(b) / 1048576.0);
    else if (b >= (1ULL << 10)) std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(b) / 1024.0);
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
    return "\"" + json_escape(s) + "\"";
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

int sample_in_list(const std::string& body, const char* key, const char* name) {
    size_t from = body.rfind("{\"ts\":");
    if (from == std::string::npos) from = 0;
    const auto at = body.find("\"" + std::string(key) + "\":[", from);
    if (at == std::string::npos) return -1;
    const auto end = body.find(']', at);
    if (end == std::string::npos) return -1;
    const auto hit = body.find("\"" + std::string(name) + "\"", at);
    return hit != std::string::npos && hit < end ? 1 : 0;
}

bool sample_has_group(const std::string& body, const char* group) {
    return sample_in_list(body, "groups", group) != 0;
}

namespace {

// The integer at `p` (as in `"id":4,`), or `fallback` when there is no
// number there or it does not fit an int.
int json_int_at(const char* p, int fallback) {
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(p, &end, 10);
    if (end == p || errno == ERANGE || v < INT_MIN || v > INT_MAX) return fallback;
    return static_cast<int>(v);
}

} // namespace

std::vector<LevelInfo> parse_levels(const std::string& body) {
    std::vector<LevelInfo> out;
    size_t pos = 0;
    while ((pos = body.find("{\"id\":", pos)) != std::string::npos) {
        const size_t end = body.find('}', pos);
        if (end == std::string::npos) break;
        const std::string obj = body.substr(pos, end - pos + 1);
        pos = end + 1;

        LevelInfo lv;
        lv.id = json_int_at(obj.c_str() + 6, 0);
        const std::string nk = "\"name\":\"";
        const size_t n = obj.find(nk);
        if (n != std::string::npos) {
            const size_t q = obj.find('"', n + nk.size());
            if (q != std::string::npos) lv.name = obj.substr(n + nk.size(), q - n - nk.size());
        }
        const size_t iv = obj.find("\"interval_ms\":");
        if (iv != std::string::npos) lv.interval_ms = json_int_at(obj.c_str() + iv + 14, 0);
        if (lv.id > 0 && !lv.name.empty()) out.push_back(lv);
    }
    return out;
}

std::vector<ProcessRow> parse_processes(const std::string& body) {
    std::vector<ProcessRow> out;
    size_t i = body.find("\"processes\":[");
    if (i == std::string::npos) return out;
    i += 13;
    // The end of the string that starts at body[at] (the opening quote):
    // the index of its closing quote, past escapes; npos if cut off.
    auto string_end = [&](size_t at) {
        for (size_t k = at + 1; k < body.size(); ++k) {
            if (body[k] == '\\') { ++k; continue; }
            if (body[k] == '"') return k;
        }
        return std::string::npos;
    };
    std::vector<ProcessRow> rows;
    while (i < body.size()) {
        while (i < body.size() && (body[i] == ',' || body[i] == ' ')) ++i;
        if (i >= body.size()) return out;
        if (body[i] == ']') return rows;
        if (body[i] != '{') return out;
        ProcessRow row;
        ++i;
        while (i < body.size() && body[i] != '}') {
            if (body[i] == ',' || body[i] == ' ') { ++i; continue; }
            if (body[i] != '"') return out;
            const size_t kend = string_end(i);
            if (kend == std::string::npos || kend + 1 >= body.size() || body[kend + 1] != ':') return out;
            const std::string key = body.substr(i + 1, kend - i - 1);
            i = kend + 2;
            if (i < body.size() && body[i] == '"') {
                const size_t vend = string_end(i);
                if (vend == std::string::npos) return out;
                const std::string v = json_unescape(body.substr(i + 1, vend - i - 1));
                if (key == "name")  row.name  = v;
                if (key == "state") row.state = v;
                i = vend + 1;
            } else {
                char* end = nullptr;
                const double v = std::strtod(body.c_str() + i, &end);
                if (end == body.c_str() + i) return out;
                if (key == "pid")         row.pid = static_cast<long>(v);
                if (key == "cpu_percent") row.cpu_percent = v;
                if (key == "rss_bytes")   row.rss_bytes = v;
                i = static_cast<size_t>(end - body.c_str());
            }
        }
        if (i >= body.size()) return out;
        ++i;                                   // '}'
        rows.push_back(row);
    }
    return out;
}

std::string fit_columns(const std::string& s, int width) {
    std::string out;
    int cols = 0;
    std::mbstate_t st{};
    size_t i = 0;
    while (i < s.size()) {
        wchar_t wc = 0;
        const size_t n = std::mbrtowc(&wc, s.data() + i, s.size() - i, &st);
        size_t take = 1;
        int    w    = 1;
        bool   ok   = false;
        if (n == static_cast<size_t>(-1) || n == static_cast<size_t>(-2) || n == 0) {
            st = std::mbstate_t{};                     // not a character here: '?'
        } else {
            const int cw = (::wcwidth)(wc);   // the function: FreeBSD's macro takes an int
            take = n;
            ok   = cw >= 0;                            // a control character: '?'
            w    = ok ? cw : 1;
        }
        if (cols + w > width) break;
        if (ok) out.append(s, i, take);
        else    out.push_back('?');
        cols += w;
        i    += take;
    }
    if (cols < width) out.append(static_cast<size_t>(width - cols), ' ');
    return out;
}

bool has_sample(const std::string& body) {
    return body.find("{\"ts\":") != std::string::npos;
}

std::string health_hostname(const std::string& body) {
    const std::string key = "\"hostname\":\"";
    const size_t at = body.find(key);
    if (at == std::string::npos) return "";
    for (size_t k = at + key.size(); k < body.size(); ++k) {
        if (body[k] == '\\') { ++k; continue; }
        if (body[k] == '"') return json_unescape(body.substr(at + key.size(), k - at - key.size()));
    }
    return "";
}

std::string level_label(const std::vector<LevelInfo>& levels, int id) {
    for (const auto& lv : levels) {
        if (lv.id != id) continue;
        const double s = lv.interval_ms / 1000.0;
        char every[32];
        if (s >= 60) std::snprintf(every, sizeof(every), "%g min", s / 60);
        else         std::snprintf(every, sizeof(every), "%g s", s);
        return lv.name + " (every " + every + ")";
    }
    return "level " + std::to_string(id);
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
        const int rc = http_request(host, port, "GET", "/api/samples?last=1", "", "", &probe);
        if (rc != 0) {
            std::fprintf(stderr, "budyk tui: can't reach %s:%d: %s\n", host, port,
                         http_error(rc).c_str());
            return -1;
        }
        if (probe.status == 401) {
            if (ask_password == nullptr) {
                std::fprintf(stderr, "budyk tui: %s:%d requires a login\n", host, port);
                return -1;
            }
            password = ask_password();
            std::string cookie;
            const int st = login(host, port, password, &cookie);
            if (st == 429) {
                std::fprintf(stderr, "budyk tui: %s:%d refuses logins for now "
                             "(too many failed attempts); try again in a minute\n",
                             host, port);
                return -1;
            }
            if (st < 0) {
                std::fprintf(stderr, "budyk tui: login to %s:%d: %s\n", host, port,
                             http_error(st).c_str());
                return -1;
            }
            if (st == 401) {
                std::fprintf(stderr, "budyk tui: login to %s:%d failed: wrong password\n",
                             host, port);
                return -1;
            }
            if (st != 200) {
                std::fprintf(stderr, "budyk tui: login to %s:%d failed: %s\n", host, port,
                             st == 0 ? "no session cookie in the reply"
                                     : ("HTTP " + std::to_string(st)).c_str());
                return -1;
            }
            cookie_header = "Cookie: " + cookie + "\r\n";
        }
    }

    // Level names for the sample's level id. Fetched again whenever a
    // sample carries an id the table doesn't know (the daemon was
    // restarted with other custom levels).
    std::vector<tui_detail::LevelInfo> levels;
    auto fetch_levels = [&] {
        tui_detail::HttpReply lr;
        if (http_request(host, port, "GET", "/api/levels", cookie_header, "", &lr) == 0 &&
            lr.status == 200) {
            levels = tui_detail::parse_levels(lr.body);
        }
    };
    fetch_levels();

    // The host's name for the title line (/api/health needs no login).
    std::string hostname;
    {
        tui_detail::HttpReply hr;
        if (http_request(host, port, "GET", "/api/health", "", "", &hr) == 0 && hr.status == 200) {
            hostname = tui_detail::health_hostname(hr.body);
        }
    }

    // --- ncurses init --------------------------------------------------------
    // The locale's character set (UTF-8): names are drawn as characters,
    // not as their bytes.
    std::setlocale(LC_ALL, "");
    ::initscr();
    ::cbreak();
    ::noecho();
    ::curs_set(0);
    ::keypad(stdscr, TRUE);
    ::wtimeout(stdscr, 1000);   // getch() blocks at most 1 s

    bool quit = false;
    int  tick = 0;
    int  levels_fetched_at = 0;

    while (!quit) {
        tui_detail::HttpReply r;
        int rc = http_request(host, port, "GET", "/api/samples?last=1", cookie_header, "", &r);
        if (rc == 0 && r.status == 401 && !password.empty()) {
            std::string cookie;                       // session expired: renew
            if (login(host, port, password, &cookie) == 200) {
                cookie_header = "Cookie: " + cookie + "\r\n";
                rc = http_request(host, port, "GET", "/api/samples?last=1", cookie_header, "", &r);
            }
        }

        ::erase();
        ::mvprintw(0, 0, "budyk tui  %s%s%s:%d   q=quit", hostname.c_str(),
                   hostname.empty() ? "" : "  ", host, port);
        ::mvprintw(0, COLS - 12, "tick #%d", tick);

        if (rc != 0) {
            ::mvprintw(2, 0, "connection error (%s) — retrying in 1s", http_error(rc).c_str());
        } else if (r.status != 200) {
            // Say what the server said instead of drawing zeros.
            ::mvprintw(2, 0, "server returned HTTP %d%s — retrying in 1s", r.status,
                       r.status == 401 ? " (login required or expired)" : "");
        } else if (!tui_detail::has_sample(r.body)) {
            // No sample yet (the daemon just started, or the hot buffer
            // was emptied): it was drawn as zeros, memory at 100 %.
            ::mvprintw(2, 0, "waiting for the first sample");
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
            const int level = static_cast<int>(sample_number(b, nullptr, "level"));
            bool known = false;
            for (const auto& lv : levels) known = known || lv.id == level;
            if (!known && tick - levels_fetched_at >= 30) {   // at most every 30 ticks
                fetch_levels();
                levels_fetched_at = tick;
            }
            ::mvprintw(2, 0, "Level  %s", tui_detail::level_label(levels, level).c_str());

            const int barw = COLS > 50 ? COLS - 40 : 10;
            // A group the level does not collect holds zeros, not readings;
            // one read for the first time has its rate from the next tick.
            auto has = [&](int row, const char* label, const char* group) {
                if (tui_detail::sample_has_group(b, group)) return true;
                ::mvprintw(row, 0, "%-6s %s", label,
                           tui_detail::sample_in_list(b, "baseline", group) == 1
                               ? "measuring..." : "not collected at this level");
                return false;
            };
            if (has(3, "CPU", "cpu"))
                ::mvprintw(3, 0, "CPU    %5.1f%% %s %u cores",
                           cpu_pct, bar(cpu_pct, barw).c_str(), cores);
            if (has(4, "Memory", "mem"))
                ::mvprintw(4, 0, "Memory %5.1f%% %s free %s / %s",
                           100.0 - mem_av, bar(100.0 - mem_av, barw).c_str(),
                           fmt_bytes(ma).c_str(), fmt_bytes(mt).c_str());
            if (has(5, "Swap", "swap"))
                ::mvprintw(5, 0, "Swap   %5.1f%% %s",
                           swap_us, bar(swap_us, barw).c_str());
            if (has(7, "Load", "load"))
                ::mvprintw(7, 0, "Load   %.2f / %.2f / %.2f", load1, load5, load15);
            if (has(8, "Disk", "disk"))
                ::mvprintw(8, 0, "Disk   r %s/s   w %s/s   (%u devs)",
                           fmt_bytes(dr).c_str(), fmt_bytes(dw).c_str(), devs);
            if (has(9, "Net", "net"))
                ::mvprintw(9, 0, "Net    rx %s/s  tx %s/s   (%u ifaces)",
                           fmt_bytes(rx).c_str(), fmt_bytes(tx).c_str(), ifs);
            const double iowait   = sample_number(b, "cpu", "iowait_percent");
            const double zombies  = sample_number(b, "proc", "zombie");
            const double blocked  = sample_number(b, "proc", "blocked");
            const double procs    = sample_number(b, "proc", "total");
            const double estab    = sample_number(b, "tcp", "established");
            const double retr     = sample_number(b, "tcp", "retransmits_per_sec");
            const double fs_used  = sample_number(b, "fs", "max_used_percent");
            const double fs_count = sample_number(b, "fs", "count");
            if (has(10, "Procs", "proc"))
                ::mvprintw(10, 0, "Procs  %.0f   %.0f blocked, %.0f zombie   iowait %.1f%%",
                           procs, blocked, zombies, iowait);
            if (has(11, "TCP", "tcp"))
                ::mvprintw(11, 0, "TCP    %.0f established   %.1f retransmits/s", estab, retr);
            if (has(12, "ICMP", "icmp"))
                ::mvprintw(12, 0, "ICMP   in %.1f/s  out %.1f/s   %.1f errors/s   %.1f unreachable/s",
                           sample_number(b, "icmp", "in_msgs_per_sec"),
                           sample_number(b, "icmp", "out_msgs_per_sec"),
                           sample_number(b, "icmp", "in_errors_per_sec"),
                           sample_number(b, "icmp", "in_dest_unreachs_per_sec"));
            if (has(13, "FS", "fs"))
                ::mvprintw(13, 0, "FS     fullest %5.1f%% %s of %.0f",
                           fs_used, bar(fs_used, barw).c_str(), fs_count);
            if (has(14, "Uptime", "uptime"))
                ::mvprintw(14, 0, "Uptime %s", fmt_uptime(uptime).c_str());

            // The top processes, as many as the window has rows for.
            const auto top = tui_detail::parse_processes(b);
            if (!top.empty() && LINES > 18) {
                ::mvprintw(16, 0, "%-24s %8s %2s %7s %9s", "process", "pid", "", "CPU", "RSS");
                for (size_t k = 0; k < top.size() && 17 + static_cast<int>(k) < LINES - 1; ++k) {
                    const auto& p = top[k];
                    // 24 columns, not 24 bytes: "%-24.24s" cut a UTF-8
                    // name inside a character and shifted the columns.
                    ::mvprintw(17 + static_cast<int>(k), 0, "%s %8ld %2.2s %6.1f%% %9s",
                               tui_detail::fit_columns(p.name, 24).c_str(), p.pid,
                               p.state.c_str(), p.cpu_percent,
                               fmt_bytes(static_cast<uint64_t>(p.rss_bytes)).c_str());
                }
            }
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
