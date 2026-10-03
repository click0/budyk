// SPDX-License-Identifier: BSD-3-Clause
#include "web/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
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

    // 2b. A multi-megabyte body arrives whole when send(2) is
    //     interrupted by signals. /api/range at its 5000-sample cap is
    //     a few MB; a blocking send on loopback writes all of it in one
    //     call unless a signal lands while it waits for buffer space —
    //     then it returns the partial count, and without the send loop
    //     the response is cut off. SIGHUP is routine, so this happens
    //     in practice. Here an interval timer sends SIGALRM (no
    //     SA_RESTART) to the server thread — the main thread blocks it
    //     — while the client drains the socket slowly so send() has to
    //     wait. The client's receive buffer is pinned small (which also
    //     turns off autotuning for it), so the body cannot sit in
    //     kernel buffers and send() has to wait on the reader.
    {
        HttpServer s;
        const std::string big(8u << 20, 'z');   // 8 MiB
        assert(s.start("127.0.0.1", 0,
                       [&big](const HttpRequest&) {
                           HttpResponse r;
                           r.status       = 200;
                           r.content_type = "text/plain";
                           r.body         = big;
                           return r;
                       }) == 0);

        struct sigaction sa{};
        sa.sa_handler = [](int) {};
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;                         // no SA_RESTART: send() returns short
        struct sigaction old_sa{};
        assert(::sigaction(SIGALRM, &sa, &old_sa) == 0);
        sigset_t alrm, prev;
        sigemptyset(&alrm);
        sigaddset(&alrm, SIGALRM);
        assert(::pthread_sigmask(SIG_BLOCK, &alrm, &prev) == 0);

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        const int rcvbuf = 64 * 1024;
        assert(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == 0);
        sockaddr_in sa_in{};
        sa_in.sin_family = AF_INET;
        sa_in.sin_port   = htons(static_cast<uint16_t>(s.bound_port()));
        ::inet_pton(AF_INET, "127.0.0.1", &sa_in.sin_addr);
        assert(::connect(fd, reinterpret_cast<sockaddr*>(&sa_in), sizeof(sa_in)) == 0);
        const std::string req = "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
        assert(::send(fd, req.data(), req.size(), 0) == static_cast<ssize_t>(req.size()));
        ::usleep(20000);                         // request parsed; server is in send()

        itimerval it{};
        it.it_interval.tv_usec = 2000;
        it.it_value.tv_usec    = 2000;
        assert(::setitimer(ITIMER_REAL, &it, nullptr) == 0);

        std::string resp;
        static char buf[64 * 1024];
        while (true) {
            ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
            if (r <= 0) break;
            resp.append(buf, static_cast<size_t>(r));
            ::usleep(1000);                      // keep the socket buffer full
        }
        ::close(fd);

        itimerval off{};
        ::setitimer(ITIMER_REAL, &off, nullptr);
        ::sigaction(SIGALRM, &old_sa, nullptr);
        ::pthread_sigmask(SIG_SETMASK, &prev, nullptr);

        const auto hdr_end = resp.find("\r\n\r\n");
        assert(hdr_end != std::string::npos);
        assert(resp.find("Content-Length: 8388608\r\n") != std::string::npos);
        assert(resp.size() - (hdr_end + 4) == big.size());
        assert(resp.compare(hdr_end + 4, big.size(), big) == 0);
        s.stop();
    }

    // 2c. A client that opens a connection and goes quiet, or drips its
    //     request a byte at a time, holds the single I/O thread for the
    //     configured budget and no longer: the next client is served
    //     after that. Without the deadline the second request would
    //     wait forever.
    {
        HttpServer s;
        s.set_io_timeout_ms(300);
        assert(s.io_timeout_ms() == 300);
        assert(s.start("127.0.0.1", 0,
                       [](const HttpRequest&) {
                           return HttpResponse{200, "text/plain", "served\n"};
                       }) == 0);
        auto connect = [&s]() {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            assert(fd >= 0);
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_port   = htons(static_cast<uint16_t>(s.bound_port()));
            ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
            assert(::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
            return fd;
        };

        // Quiet client: half a request line, then nothing.
        int quiet = connect();
        assert(::send(quiet, "GET / HT", 8, 0) == 8);
        ::usleep(50000);                          // make sure it is being served
        auto t0 = std::chrono::steady_clock::now();
        std::string resp = http_round_trip(s.bound_port(),
            "GET /after-quiet HTTP/1.1\r\nHost: x\r\n\r\n");
        double secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(secs < 2.0);
        char one;
        assert(::recv(quiet, &one, 1, 0) == 0);   // server closed it, no response
        ::close(quiet);

        // Dripping client: a byte every 100 ms, each within the socket
        // timeout; only the whole-request deadline stops it.
        int drip = connect();
        const std::string line = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        size_t sent = 0;
        t0 = std::chrono::steady_clock::now();
        while (sent < line.size() &&
               ::send(drip, line.data() + sent, 1, MSG_NOSIGNAL) == 1) {
            ++sent;
            ::usleep(100000);
            if (::recv(drip, &one, 1, MSG_DONTWAIT) == 0) break;   // server gave up
        }
        secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        assert(sent < line.size());               // cut off before the request completed
        assert(secs < 2.0);
        ::close(drip);

        resp = http_round_trip(s.bound_port(),
            "GET /after-drip HTTP/1.1\r\nHost: x\r\n\r\n");
        assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);

        // A body that never arrives in full is cut off too.
        int half = connect();
        const std::string partial =
            "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nonly-this";
        assert(::send(half, partial.data(), partial.size(), 0) ==
               static_cast<ssize_t>(partial.size()));
        t0 = std::chrono::steady_clock::now();
        assert(::recv(half, &one, 1, 0) == 0);    // closed, no 200
        secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        assert(secs < 2.0);
        ::close(half);
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

    // 7. Headers — case-insensitive lookup and multi-header support.
    {
        HttpServer s;
        assert(s.start("127.0.0.1", 0,
                       [](const HttpRequest& req) {
                           HttpResponse r;
                           r.status       = 200;
                           r.content_type = "text/plain";
                           r.body         = req.header("X-Foo") + "|" +
                                            req.header("Cookie");
                           return r;
                       }) == 0);
        std::string resp = http_round_trip(s.bound_port(),
            "GET / HTTP/1.1\r\nHost: x\r\nx-foo: bar\r\nCookie: a=1; b=2\r\n\r\n");
        assert(resp.find("\r\n\r\nbar|a=1; b=2") != std::string::npos);
        s.stop();
    }

    // 8. Body — Content-Length-driven, exact byte count delivered to handler.
    {
        HttpServer s;
        assert(s.start("127.0.0.1", 0,
                       [](const HttpRequest& req) {
                           HttpResponse r;
                           r.status       = 200;
                           r.content_type = "application/json";
                           r.body         = "got=" + req.body;
                           return r;
                       }) == 0);
        std::string resp = http_round_trip(s.bound_port(),
            "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 13\r\n\r\nhello, server");
        assert(resp.find("\r\n\r\ngot=hello, server") != std::string::npos);
        s.stop();
    }

    // 9. Extra response headers — Set-Cookie shows up verbatim.
    {
        HttpServer s;
        assert(s.start("127.0.0.1", 0,
                       [](const HttpRequest&) {
                           HttpResponse r;
                           r.status       = 200;
                           r.content_type = "text/plain";
                           r.body         = "ok";
                           r.extra_headers.push_back({"Set-Cookie",
                               "budyk_session=deadbeef; HttpOnly; Path=/"});
                           return r;
                       }) == 0);
        std::string resp = http_round_trip(s.bound_port(),
            "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        assert(resp.find("Set-Cookie: budyk_session=deadbeef; HttpOnly; Path=/") !=
               std::string::npos);
        s.stop();
    }

    // 10. Oversized body rejected with 413.
    {
        HttpServer s;
        assert(s.start("127.0.0.1", 0,
                       [](const HttpRequest&) {
                           return HttpResponse{200, "text/plain", "ok"};
                       }) == 0);
        std::string resp = http_round_trip(s.bound_port(),
            "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1000000\r\n\r\n");
        assert(resp.find("HTTP/1.1 413 Payload Too Large") != std::string::npos);
        s.stop();
    }

    std::printf("test_http_server: PASS\n");
    return 0;
}
