// SPDX-License-Identifier: BSD-3-Clause
#include "web/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace budyk {

namespace {

// Read until "\r\n\r\n" (end of HTTP headers) or until cap is reached
// or the peer closes. Returns the number of bytes read; -1 on error.
ssize_t read_request_headers(int fd, char* buf, size_t cap) {
    size_t total = 0;
    while (total < cap) {
        ssize_t n = ::recv(fd, buf + total, cap - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += static_cast<size_t>(n);
        if (total >= 4) {
            for (size_t i = 0; i + 3 < total; ++i) {
                if (std::memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                    return static_cast<ssize_t>(total);
                }
            }
        }
    }
    return static_cast<ssize_t>(total);
}

// Pull METHOD and PATH out of the first line ("METHOD PATH HTTP/1.x").
bool parse_request_line(const char* buf, size_t len, HttpRequest* out) {
    const char* eol = static_cast<const char*>(std::memchr(buf, '\r', len));
    if (eol == nullptr) return false;
    const size_t llen = static_cast<size_t>(eol - buf);

    const char* sp1 = static_cast<const char*>(std::memchr(buf, ' ', llen));
    if (sp1 == nullptr) return false;
    const char* sp2 = static_cast<const char*>(
        std::memchr(sp1 + 1, ' ', llen - (sp1 + 1 - buf)));
    if (sp2 == nullptr) return false;

    out->method.assign(buf, sp1);
    out->path  .assign(sp1 + 1, sp2);
    return true;
}

const char* status_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

// Write the entire response in one shot. Connection: close avoids us
// having to deal with keep-alive framing.
ssize_t send_response(int fd, const HttpResponse& r) {
    char hdr[512];
    int n = std::snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        r.status, status_phrase(r.status),
        r.content_type.empty() ? "text/plain" : r.content_type.c_str(),
        r.body.size());
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(hdr)) return -1;

    ssize_t w = ::send(fd, hdr, static_cast<size_t>(n), 0);
    if (w != n) return -1;
    if (!r.body.empty()) {
        w = ::send(fd, r.body.data(), r.body.size(), 0);
        if (w != static_cast<ssize_t>(r.body.size())) return -1;
    }
    return n + static_cast<ssize_t>(r.body.size());
}

} // namespace

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() { stop(); }

int HttpServer::start(const char* listen_addr, int port, HttpHandler handler) {
    if (running_.load() || listen_fd_ >= 0) return -1;
    if (listen_addr == nullptr || handler == nullptr) return -2;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
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
    if (::listen(fd, 16) != 0) {
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
        ::shutdown(listen_fd_, SHUT_RDWR);  // unblock accept() on Linux
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (loop_.joinable()) loop_.join();
}

int HttpServer::bound_port() const { return bound_port_; }

void HttpServer::run_loop() {
    while (running_.load()) {
        sockaddr_in cli{};
        socklen_t   clen = sizeof(cli);
        int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            // Listening fd closed by stop() — exit cleanly.
            break;
        }
        handle_client(cfd);
        ::close(cfd);
    }
}

void HttpServer::handle_client(int client_fd) {
    char buf[4096];
    ssize_t n = read_request_headers(client_fd, buf, sizeof(buf));
    if (n <= 0) return;

    HttpRequest req;
    if (!parse_request_line(buf, static_cast<size_t>(n), &req)) {
        HttpResponse bad{400, "text/plain", "bad request\n"};
        send_response(client_fd, bad);
        return;
    }
    HttpResponse resp = handler_(req);
    send_response(client_fd, resp);
}

} // namespace budyk
