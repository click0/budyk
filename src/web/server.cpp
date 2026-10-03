// SPDX-License-Identifier: BSD-3-Clause
#include "web/server.h"

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

// Read into `buf` until "\r\n\r\n" is seen. Returns the index where
// the body would start (i.e. byte after the marker). 0 on EOF before
// finding the marker, -1 on error or when the deadline passes.
ssize_t read_until_headers(int fd, std::vector<char>* buf, Deadline deadline) {
    buf->resize(0);
    char tmp[1024];
    while (buf->size() < kHeaderReadCap) {
        ssize_t n = recv_bounded(fd, tmp, sizeof(tmp), deadline);
        if (n < 0) return -1;
        if (n == 0) return 0;
        buf->insert(buf->end(), tmp, tmp + n);
        // Search just the new chunk plus 3-byte overlap.
        const size_t end = buf->size();
        for (size_t i = (end > static_cast<size_t>(n) + 3 ? end - n - 3 : 0);
             i + 3 < end; ++i) {
            if ((*buf)[i] == '\r' && (*buf)[i + 1] == '\n' &&
                (*buf)[i + 2] == '\r' && (*buf)[i + 3] == '\n') {
                return static_cast<ssize_t>(i + 4);
            }
        }
    }
    return -1;
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

bool ieq(const std::string& a, const char* b) {
    const size_t blen = std::strlen(b);
    if (a.size() != blen) return false;
    for (size_t i = 0; i < blen; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

void trim_inplace(std::string* s) {
    size_t b = 0;
    while (b < s->size() && std::isspace(static_cast<unsigned char>((*s)[b]))) ++b;
    size_t e = s->size();
    while (e > b && std::isspace(static_cast<unsigned char>((*s)[e - 1]))) --e;
    if (b > 0 || e < s->size()) *s = s->substr(b, e - b);
}

// Pull METHOD and PATH out of the first line ("METHOD PATH HTTP/1.x"),
// then walk the remaining header lines into `req->headers`.
bool parse_headers(const char* buf, size_t end_offset, HttpRequest* req) {
    // First line
    const char* eol = static_cast<const char*>(std::memchr(buf, '\r', end_offset));
    if (eol == nullptr) return false;
    const size_t llen = static_cast<size_t>(eol - buf);

    const char* sp1 = static_cast<const char*>(std::memchr(buf, ' ', llen));
    if (sp1 == nullptr) return false;
    const char* sp2 = static_cast<const char*>(
        std::memchr(sp1 + 1, ' ', llen - (sp1 + 1 - buf)));
    if (sp2 == nullptr) return false;
    req->method.assign(buf, sp1);
    req->path  .assign(sp1 + 1, sp2);

    // Subsequent header lines
    size_t pos = llen + 2;                 // skip "\r\n"
    while (pos + 1 < end_offset - 2) {     // up to the trailing "\r\n\r\n"
        const char* line = buf + pos;
        const char* line_eol = static_cast<const char*>(
            std::memchr(line, '\r', end_offset - pos));
        if (line_eol == nullptr) break;
        const size_t line_len = static_cast<size_t>(line_eol - line);
        if (line_len == 0) break;

        const char* colon = static_cast<const char*>(std::memchr(line, ':', line_len));
        if (colon != nullptr) {
            std::string key  (line,        colon);
            std::string value(colon + 1,   line + line_len);
            trim_inplace(&value);
            req->headers.emplace_back(std::move(key), std::move(value));
        }
        pos += line_len + 2;
    }
    return true;
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
        case 500: return "Internal Server Error";
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
        if (ieq(kv.first, name.c_str())) return kv.second;
    }
    return {};
}

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() { stop(); }

int HttpServer::start(const char* listen_addr, int port, HttpHandler handler) {
    if (running_.load() || listen_fd_ >= 0) return -1;
    if (listen_addr == nullptr || handler == nullptr) return -2;

    // CLOEXEC: rule exec() and the alert channels fork; a child must
    // not inherit the listening socket (it would keep the port bound
    // after a restart) or a client connection.
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -3;

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
        ::close(fd);
        return -5;
    }
    // Connections wait here while the single thread serves another;
    // each one it serves is bounded by io_timeout_ms_, so the queue
    // drains even under a stalled client.
    if (::listen(fd, 64) != 0) {
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

void HttpServer::set_io_timeout_ms(int ms) { io_timeout_ms_ = ms > 0 ? ms : 1; }
int  HttpServer::io_timeout_ms() const     { return io_timeout_ms_; }

void HttpServer::run_loop() {
    while (running_.load()) {
        sockaddr_in cli{};
        socklen_t   clen = sizeof(cli);
        int cfd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &clen,
                            SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const bool hijacked = handle_client(cfd);
        if (!hijacked) ::close(cfd);
    }
}

bool HttpServer::handle_client(int client_fd) {
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
    if (hdr_end <= 0) return false;

    HttpRequest req;
    if (!parse_headers(buf.data(), static_cast<size_t>(hdr_end), &req)) {
        send_response(client_fd, HttpResponse{400, "text/plain", "bad request\n"});
        return false;
    }

    // Prefix bytes already in `buf` past the headers belong to the body.
    if (static_cast<size_t>(hdr_end) < buf.size()) {
        req.body.assign(buf.data() + hdr_end, buf.size() - hdr_end);
    }

    // If Content-Length is set, pull the rest of the body off the wire.
    const std::string cl = req.header("Content-Length");
    if (!cl.empty()) {
        char* endp = nullptr;
        unsigned long want = std::strtoul(cl.c_str(), &endp, 10);
        if (endp == cl.c_str() || want > kMaxBodyBytes) {
            send_response(client_fd, HttpResponse{413, "text/plain", "body too large\n"});
            return false;
        }
        if (req.body.size() < want) {
            const size_t need = want - req.body.size();
            std::vector<char> rest(need);
            ssize_t got = read_full(client_fd, rest.data(), need, deadline);
            if (got < 0) return false;             // stalled mid-body
            if (got > 0) req.body.append(rest.data(), static_cast<size_t>(got));
        } else if (req.body.size() > want) {
            req.body.resize(want);
        }
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
