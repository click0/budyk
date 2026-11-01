// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <sys/types.h>

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace budyk {

/// @file
/// Embedded HTTP/1.1 server (spec §3.7). Sequential accept-handle-close
/// per connection, single I/O thread; suitable for the single-admin
/// monitoring use case. Every connection is bounded by io_timeout_ms()
/// so a client that stalls cannot hold the thread. The thread is owned by the server — start()
/// returns once the listening socket is bound, stop() joins.
///
/// Routing is a flat std::function dispatcher set by the caller before
/// start(). The handler is invoked on the I/O thread; if you need to
/// call into shared state, the handler is responsible for the
/// synchronisation.

struct HttpRequest {
    std::string method;       ///< "GET", "POST", ...
    std::string path;         ///< "/api/health" — the target up to '?'
    std::string query;        ///< what follows '?', raw (not percent-decoded)
    std::string peer;         ///< client address, dotted quad ("127.0.0.1")
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// Case-insensitive header lookup. Returns "" if missing. Header
    /// names follow RFC 7230 ASCII conventions.
    std::string header(const std::string& name) const;
};

struct HttpResponse {
    HttpResponse() = default;
    HttpResponse(int s, std::string type, std::string text)
        : status(s), content_type(std::move(type)), body(std::move(text)) {}

    int         status = 200; ///< 200, 404, ...
    std::string content_type; ///< "application/json", "text/plain"
    std::string body;
    /// Extra response headers — written verbatim after Content-Length.
    /// Use for things like Set-Cookie, Cache-Control, etc.
    std::vector<std::pair<std::string, std::string>> extra_headers;
    /// If set, the server hands the live socket fd to this callback
    /// INSTEAD of writing a normal response. The callback owns the fd
    /// (must close it eventually) — used for the WebSocket upgrade and
    /// other long-lived connection takeovers.
    std::function<void(int fd)> hijack;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

/// send(2) `len` bytes to `fd` within `io_timeout_ms` plus the size at
/// `min_rate` bytes per second, waiting for room with poll(): the bytes
/// sent, or -1 when the peer is gone or reads slower than that. The
/// bound a response gets, for what a hijack callback writes before the
/// socket is its own (the WebSocket handshake and the catch-up frame):
/// a blocking send() there held the HTTP thread for as long as a client
/// cared to read a byte at a time (on FreeBSD SO_SNDTIMEO bounds each
/// wait for room, not the call).
ssize_t send_bounded(int fd, const void* data, size_t len, int io_timeout_ms, size_t min_rate);

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    /// Bind and start the accept loop on a background thread. The
    /// listen address may be "127.0.0.1", "0.0.0.0", or any IPv4
    /// dotted-quad. Pass port == 0 to let the kernel pick a free port,
    /// which can then be read with bound_port() once start() returns 0.
    int  start(const char* listen_addr, int port, HttpHandler handler);

    /// Bring the loop down. Closes the listening socket — any in-flight
    /// accept returns EBADF and the loop exits — then joins the thread.
    void stop();

    /// Real bound port (useful when start was called with port == 0).
    int  bound_port() const;

    /// Why start() failed: describe() names the step (-3 socket, -4 bad
    /// address, -5 bind, -6 listen, -7 the thread), last_errno() is that
    /// step's errno (0 for -1/-2/-4). "bind: Address already in use"
    /// beats "rc=-5". The thread is made with pthread_create(), not
    /// std::thread: with exceptions off, the std::system_error that one
    /// throws when it cannot (EAGAIN) ended the daemon.
    static const char* describe(int rc);
    int  last_errno() const;

    /// How long one request may take to arrive (headers and body) and
    /// how long a send may wait on the client, in milliseconds; default
    /// 5000. A connection that is still sending its request when the
    /// budget runs out is closed without a response, so one client that
    /// opens a connection and goes quiet holds the single I/O thread for
    /// at most this long, not forever. Set before start().
    void set_io_timeout_ms(int ms);
    int  io_timeout_ms() const;
    /// A response may take io_timeout_ms() plus its body's size at this
    /// many bytes per second (default 256 KiB/s, so a few MB of
    /// /api/range gets a few more seconds); a client reading slower is
    /// cut off, so it cannot hold the thread.
    void set_min_send_rate(size_t bytes_per_sec);
    size_t min_send_rate() const;

private:
    /// Written by start() and stop() on the caller's thread, read by the
    /// loop thread: atomic so the two never race (ThreadSanitizer).
    std::atomic<int>       listen_fd_{-1};
    int                    bound_port_  = 0;
    int                    io_timeout_ms_ = 5000;
    size_t                 min_send_rate_ = 256 * 1024;
    int                    last_errno_    = 0;
    int                    last_accept_errno_ = 0;   ///< logged once, until an accept succeeds
    pthread_t              loop_{};
    bool                   loop_started_ = false;    ///< loop_ runs, or waits to be joined
    std::atomic<bool>      running_{false};
    HttpHandler            handler_;

    void run_loop();
    static void* run_thread(void* self);
    /// Returns true if the handler set HttpResponse::hijack and the fd
    /// ownership has moved out — the run loop must NOT close it.
    bool handle_client(int client_fd, const char* peer);
};

} // namespace budyk
