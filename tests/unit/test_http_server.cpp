// SPDX-License-Identifier: BSD-3-Clause
#include "web/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace budyk;

// Open a TCP connection to the given port, send `request`, and slurp
// the entire response into a string until the peer closes.
static std::string http_round_trip(int port, const std::string& request) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);

    ssize_t w = ::send(fd, request.data(), request.size(), 0);
    assert(w == static_cast<ssize_t>(request.size()));

    std::string out;
    char buf[1024];
    while (true) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        out.append(buf, static_cast<size_t>(r));
    }
    ::close(fd);
    return out;
}

int main() {
    // 1. start() rejects null args.
    {
        HttpServer s;
        assert(s.start(nullptr, 0, [](const HttpRequest&) {
            return HttpResponse{200, "text/plain", "ok"};
        }) != 0);
        assert(s.start("127.0.0.1", 0, nullptr) != 0);
    }

    // 2. Round-trip — handler returns a static body. The kernel hands
    //    out the port (port == 0 → bound_port() reveals it).
    {
        HttpServer s;
        int rc = s.start("127.0.0.1", 0,
                         [](const HttpRequest& req) {
                             HttpResponse r;
                             r.status       = 200;
                             r.content_type = "application/json";
                             r.body         = "{\"path\":\"" + req.path +
                                              "\",\"method\":\"" + req.method + "\"}";
                             return r;
                         });
        assert(rc == 0);
        assert(s.bound_port() > 0);

        std::string resp = http_round_trip(s.bound_port(),
            "GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n");

        assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(resp.find("Content-Type: application/json") != std::string::npos);
        assert(resp.find("\"path\":\"/api/health\"") != std::string::npos);
        assert(resp.find("\"method\":\"GET\"") != std::string::npos);

        s.stop();
    }

    // 3. Handler can return 404 — the server passes it through.
    {
        HttpServer s;
        assert(s.start("127.0.0.1", 0,
                       [](const HttpRequest& req) {
                           if (req.path == "/health") {
                               return HttpResponse{200, "text/plain", "ok\n"};
                           }
                           return HttpResponse{404, "text/plain", "no\n"};
                       }) == 0);

        std::string ok = http_round_trip(s.bound_port(),
            "GET /health HTTP/1.1\r\n\r\n");
        std::string no = http_round_trip(s.bound_port(),
            "GET /missing HTTP/1.1\r\n\r\n");
        assert(ok.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(no.find("HTTP/1.1 404 Not Found") != std::string::npos);

        s.stop();
    }

    // 4. start() rejects double-start on the same instance.
    {
        HttpServer s;
        auto h = [](const HttpRequest&) {
            return HttpResponse{200, "text/plain", "ok"};
        };
        assert(s.start("127.0.0.1", 0, h) == 0);
        assert(s.start("127.0.0.1", 0, h) != 0);
        s.stop();
    }

    // 5. stop() is idempotent.
    {
        HttpServer s;
        s.stop();   // never started — should be a no-op
        assert(s.start("127.0.0.1", 0,
                       [](const HttpRequest&) {
                           return HttpResponse{200, "text/plain", "ok"};
                       }) == 0);
        s.stop();
        s.stop();   // second stop — no-op
    }

    // 6. Bad request line (no spaces at all) — server replies 400.
    {
        HttpServer s;
        assert(s.start("127.0.0.1", 0,
                       [](const HttpRequest&) {
                           return HttpResponse{200, "text/plain", "ok"};
                       }) == 0);

        std::string resp = http_round_trip(s.bound_port(),
            "GARBAGE_NO_SPACES_HERE\r\n\r\n");
        assert(resp.find("HTTP/1.1 400 Bad Request") != std::string::npos);
        s.stop();
    }

    std::printf("test_http_server: PASS\n");
    return 0;
}
