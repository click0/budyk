// SPDX-License-Identifier: BSD-3-Clause
#include "web/server.h"
#include "web/http_util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace budyk {

namespace {

constexpr size_t kHeaderReadCap = 16 * 1024;   // 16 KiB header window
constexpr size_t kMaxBodyBytes  = 64 * 1024;   // 64 KiB body cap

using Clock    = std::chrono::steady_clock;
using Deadline = Clock::time_point;

// recv(2) with the per-request budget applied twice over: SO_RCVTIMEO
// on the socket bounds one call (a client that sends nothing), and the
// deadline bounds the whole request (a client that drips one byte at a
// time, each within the socket timeout). Returns -1 on either.
ssize_t recv_bounded(int fd, char* dst, size_t cap, Deadline deadline) {
    for (;;) {
        if (Clock::now() >= deadline) return -1;
        const ssize_t n = ::recv(fd, dst, cap, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;   // SO_RCVTIMEO
        return n;
    }
}

// Index one past the blank line that ends the header block, or 0 if
// buf[from, n) holds none yet. A line ends in CRLF or a bare LF (RFC 7230
// §3.5 lets a server accept the latter), so the block ends at the first
// "\n\n", "\n\r\n" (which covers "\r\n\r\n") after `from`.
size_t find_header_end(const std::vector<char>& b, size_t from) {
    for (size_t i = from; i + 1 < b.size(); ++i) {
        if (b[i] != '\n') continue;
        if (b[i + 1] == '\n') return i + 2;
        if (b[i + 1] == '\r' && i + 2 < b.size() && b[i + 2] == '\n') return i + 3;
    }
    return 0;
}

constexpr ssize_t kHeadersTooLarge = -2;

// Read into `buf` until the header block is complete. Returns the index
// where the body starts (one past the blank line); 0 on EOF before that;
// -1 on error or when the deadline passes; kHeadersTooLarge when the
// block outgrows kHeaderReadCap.
ssize_t read_until_headers(int fd, std::vector<char>* buf, Deadline deadline) {
    buf->resize(0);
    char tmp[1024];
    while (buf->size() < kHeaderReadCap) {
        ssize_t n = recv_bounded(fd, tmp, sizeof(tmp), deadline);
        if (n < 0) return -1;
        if (n == 0) return 0;
        // Search the new chunk plus a 2-byte overlap with the old data.
        const size_t from = buf->size() > 2 ? buf->size() - 2 : 0;
        buf->insert(buf->end(), tmp, tmp + n);
        const size_t end = find_header_end(*buf, from);
        if (end != 0) return static_cast<ssize_t>(end);
    }
    return kHeadersTooLarge;
}

ssize_t read_full(int fd, char* dst, size_t want, Deadline deadline) {
    size_t total = 0;
    while (total < want) {
        ssize_t n = recv_bounded(fd, dst + total, want - total, deadline);
        if (n < 0) return -1;
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

void trim_inplace(std::string* s) {
    size_t b = 0;
    while (b < s->size() && std::isspace(static_cast<unsigned char>((*s)[b]))) ++b;
    size_t e = s->size();
    while (e > b && std::isspace(static_cast<unsigned char>((*s)[e - 1]))) --e;
    if (b > 0 || e < s->size()) *s = s->substr(b, e - b);
}

// One line of the header block, without its CRLF / LF.
struct Line { const char* p; size_t n; };

// Parse the header block buf[0, end) — request line, then header lines —
// into req. False on anything that is not a well-formed HTTP/1.x request
// head: the caller answers 400.
//   request line  METHOD SP target SP HTTP/1.x, the method in A-Z, the
//                 target starting with '/'; the target is split into
//                 path and query at the first '?'.
//   header line   name ":" value, the name non-empty with no whitespace
//                 (RFC 7230 §3.2.4: whitespace before the colon is an
//                 error, and a line that starts with whitespace is
//                 obsolete folding, which this server does not accept).
bool parse_headers(const char* buf, size_t end, HttpRequest* req) {
    std::vector<Line> lines;
    size_t pos = 0;
    while (pos < end) {
        const char* nl = static_cast<const char*>(std::memchr(buf + pos, '\n', end - pos));
        if (nl == nullptr) break;
        size_t n = static_cast<size_t>(nl - (buf + pos));
        if (n > 0 && buf[pos + n - 1] == '\r') --n;
        lines.push_back(Line{buf + pos, n});
        pos = static_cast<size_t>(nl - buf) + 1;
    }
    // The last line is the blank one that ends the block.
    if (lines.size() < 2 || lines.back().n != 0) return false;
    lines.pop_back();

    const Line& rl = lines.front();
    const char* sp1 = static_cast<const char*>(std::memchr(rl.p, ' ', rl.n));
    if (sp1 == nullptr || sp1 == rl.p) return false;
    const size_t rest = rl.n - static_cast<size_t>(sp1 + 1 - rl.p);
    const char* sp2 = static_cast<const char*>(std::memchr(sp1 + 1, ' ', rest));
    if (sp2 == nullptr || sp2 == sp1 + 1) return false;
    const std::string version(sp2 + 1, rl.p + rl.n);
    if (version.compare(0, 7, "HTTP/1.") != 0 || version.size() != 8) return false;
    req->method.assign(rl.p, sp1);
    for (char c : req->method) {
        if (c < 'A' || c > 'Z') return false;
    }
    const std::string target(sp1 + 1, sp2);
    if (target.empty() || target[0] != '/') return false;
    split_target(target, &req->path, &req->query);

    for (size_t i = 1; i < lines.size(); ++i) {
        const Line& l = lines[i];
        if (l.n == 0) return false;
        if (l.p[0] == ' ' || l.p[0] == '\t') return false;            // obs-fold
        const char* colon = static_cast<const char*>(std::memchr(l.p, ':', l.n));
        if (colon == nullptr || colon == l.p) return false;
        std::string key(l.p, colon);
        for (char c : key) {
            if (c == ' ' || c == '\t') return false;
        }
        std::string value(colon + 1, l.p + l.n);
        trim_inplace(&value);
        req->headers.emplace_back(std::move(key), std::move(value));
    }
    return true;
}

// The body length a request declares. Every Content-Length header must
// be all digits and they must agree (RFC 7230 §3.3.2: differing values
// are an error — a request-smuggling vector behind a proxy). -1 when a
// header is malformed or they disagree; *present tells whether any was
// sent.
long long declared_length(const HttpRequest& req, bool* present) {
    *present = false;
    long long value = -1;
    for (const auto& kv : req.headers) {
        if (!ascii_ieq(kv.first, "Content-Length")) continue;
        const std::string& v = kv.second;
        if (v.empty() || v.size() > 18) return -1;
        long long n = 0;
        for (char c : v) {
            if (c < '0' || c > '9') return -1;
            n = n * 10 + (c - '0');
        }
        if (*present && n != value) return -1;
        *present = true;
        value    = n;
    }
    return *present ? value : 0;
}

const char* status_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default:  return "OK";
    }
}

// send(2) until everything is written or the peer is gone. A large
// body (/api/range at its 5000-sample cap is a few MB) rarely goes out
// in one call, and a signal (SIGHUP is routine) returns short too.
// MSG_NOSIGNAL: a client that hung up mid-response is a failed send,
// not SIGPIPE to the daemon.
ssize_t send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t total = 0;
    while (total < len) {
        const ssize_t n = ::send(fd, p + total, len - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

// Serialise the response head into a single string, then send body.
ssize_t send_response(int fd, const HttpResponse& r) {
    std::string head;
    head.reserve(256 + r.extra_headers.size() * 64);
    char line[256];
    int n = std::snprintf(line, sizeof(line),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n",
        r.status, status_phrase(r.status),
        r.content_type.empty() ? "text/plain" : r.content_type.c_str(),
        r.body.size());
    if (n <= 0) return -1;
    head.append(line, static_cast<size_t>(n));

    for (const auto& kv : r.extra_headers) {
        head.append(kv.first);
        head.append(": ");
        head.append(kv.second);
        head.append("\r\n");
    }
    head.append("\r\n");

    if (send_all(fd, head.data(), head.size()) < 0) return -1;
    if (!r.body.empty() && send_all(fd, r.body.data(), r.body.size()) < 0) return -1;
    return static_cast<ssize_t>(head.size() + r.body.size());
}

} // namespace

std::string HttpRequest::header(const std::string& name) const {
    for (const auto& kv : headers) {
        if (ascii_ieq(kv.first, name.c_str())) return kv.second;
    }
    return {};
}

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() { stop(); }

int HttpServer::start(const char* listen_addr, int port, HttpHandler handler) {
    last_errno_ = 0;
    if (running_.load() || listen_fd_ >= 0) return -1;
    if (listen_addr == nullptr || handler == nullptr) return -2;

    // CLOEXEC: rule exec() and the alert channels fork; a child must
    // not inherit the listening socket (it would keep the port bound
    // after a restart) or a client connection.
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { last_errno_ = errno; return -3; }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, listen_addr, &sa.sin_addr) != 1) {
        ::close(fd);
        return -4;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        last_errno_ = errno;
        ::close(fd);
        return -5;
    }
    // Connections wait here while the single thread serves another;
    // each one it serves is bounded by io_timeout_ms_, so the queue
    // drains even under a stalled client.
    if (::listen(fd, 64) != 0) {
        last_errno_ = errno;
        ::close(fd);
        return -6;
    }

    sockaddr_in bound{};
    socklen_t   bound_len = sizeof(bound);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len);
    bound_port_ = ntohs(bound.sin_port);
    listen_fd_  = fd;
    handler_    = std::move(handler);
    running_.store(true);

    loop_ = std::thread([this] { run_loop(); });
    return 0;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) {
        if (loop_.joinable()) loop_.join();
        return;
    }
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (loop_.joinable()) loop_.join();
}

int HttpServer::bound_port() const { return bound_port_; }

int HttpServer::last_errno() const { return last_errno_; }

const char* HttpServer::describe(int rc) {
    switch (rc) {
        case 0:  return "ok";
        case -1: return "already started";
        case -2: return "no address or no handler";
        case -3: return "socket";
        case -4: return "not an IPv4 address";
        case -5: return "bind";
        case -6: return "listen";
        default: return "unknown error";
    }
}

void HttpServer::set_io_timeout_ms(int ms) { io_timeout_ms_ = ms > 0 ? ms : 1; }
int  HttpServer::io_timeout_ms() const     { return io_timeout_ms_; }

void HttpServer::run_loop() {
    // Read once: stop() closes the socket and writes -1 to listen_fd_
    // from another thread, and the loop must not read the member while
    // that happens (ThreadSanitizer reported the race). accept4 on the
    // closed fd fails with EBADF and the loop ends.
    const int lfd = listen_fd_;
    while (running_.load()) {
        sockaddr_in cli{};
        socklen_t   clen = sizeof(cli);
        int cfd = ::accept4(lfd, reinterpret_cast<sockaddr*>(&cli), &clen,
                            SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        char peer[INET_ADDRSTRLEN] = "";
        ::inet_ntop(AF_INET, &cli.sin_addr, peer, sizeof(peer));
        const bool hijacked = handle_client(cfd, peer);
        if (!hijacked) ::close(cfd);
    }
}

bool HttpServer::handle_client(int client_fd, const char* peer) {
    // Per-call socket timeouts plus a whole-request deadline; see
    // recv_bounded. The send timeout also covers the response, and the
    // handshake and catch-up frame a WebSocket hijack sends before the
    // hub takes the fd non-blocking.
    timeval tv{};
    tv.tv_sec  = io_timeout_ms_ / 1000;
    tv.tv_usec = (io_timeout_ms_ % 1000) * 1000;
    ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const Deadline deadline = Clock::now() + std::chrono::milliseconds(io_timeout_ms_);

    std::vector<char> buf;
    ssize_t hdr_end = read_until_headers(client_fd, &buf, deadline);
    if (hdr_end == kHeadersTooLarge) {
        send_response(client_fd, HttpResponse{431, "text/plain", "request headers too large\n"});
        return false;
    }
    if (hdr_end <= 0) return false;

    HttpRequest req;
    req.peer = peer != nullptr ? peer : "";
    if (!parse_headers(buf.data(), static_cast<size_t>(hdr_end), &req)) {
        send_response(client_fd, HttpResponse{400, "text/plain", "bad request\n"});
        return false;
    }

    // Prefix bytes already in `buf` past the headers belong to the body.
    if (static_cast<size_t>(hdr_end) < buf.size()) {
        req.body.assign(buf.data() + hdr_end, buf.size() - static_cast<size_t>(hdr_end));
    }

    // No chunked (or any other) transfer coding: say so rather than read
    // a body of unknown length (RFC 7230 §3.3.1).
    if (!req.header("Transfer-Encoding").empty()) {
        send_response(client_fd, HttpResponse{501, "text/plain",
                                              "transfer-encoding not supported\n"});
        return false;
    }
    bool has_length = false;
    const long long want = declared_length(req, &has_length);
    if (want < 0) {
        send_response(client_fd, HttpResponse{400, "text/plain", "bad content-length\n"});
        return false;
    }
    if (static_cast<unsigned long long>(want) > kMaxBodyBytes) {
        send_response(client_fd, HttpResponse{413, "text/plain", "body too large\n"});
        return false;
    }
    // Pull the rest of the body off the wire; drop anything past it.
    const size_t len = static_cast<size_t>(want);
    if (req.body.size() < len) {
        const size_t need = len - req.body.size();
        std::vector<char> rest(need);
        ssize_t got = read_full(client_fd, rest.data(), need, deadline);
        if (got < 0) return false;             // stalled mid-body
        if (got > 0) req.body.append(rest.data(), static_cast<size_t>(got));
    } else if (req.body.size() > len) {
        req.body.resize(len);
    }

    HttpResponse resp = handler_(req);
    if (resp.hijack) {
        // The handler owns the fd from here on — typically a WS upgrade.
        resp.hijack(client_fd);
        return true;
    }
    send_response(client_fd, resp);
    return false;
}

} // namespace budyk
