// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace budyk {

// Embedded HTTP/1.1 server (spec §3.7). Sequential accept-handle-close
// per connection, single I/O thread; suitable for the single-admin
// monitoring use case. The thread is owned by the server — start()
// returns once the listening socket is bound, stop() joins.
//
// Routing is a flat std::function dispatcher set by the caller before
// start(). The handler is invoked on the I/O thread; if you need to
// call into shared state, the handler is responsible for the
// synchronisation.

struct HttpRequest {
    std::string method;       // "GET", "POST", ...
    std::string path;         // "/api/health"
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    // Case-insensitive header lookup. Returns "" if missing. Header
    // names follow RFC 7230 ASCII conventions.
    std::string header(const std::string& name) const;
};

struct HttpResponse {
    int         status;       // 200, 404, ...
    std::string content_type; // "application/json", "text/plain"
    std::string body;
    // Extra response headers — written verbatim after Content-Length.
    // Use for things like Set-Cookie, Cache-Control, etc.
    std::vector<std::pair<std::string, std::string>> extra_headers;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    // Bind and start the accept loop on a background thread. The
    // listen address may be "127.0.0.1", "0.0.0.0", or any IPv4
    // dotted-quad. Pass port == 0 to let the kernel pick a free port,
    // which can then be read with bound_port() once start() returns 0.
    int  start(const char* listen_addr, int port, HttpHandler handler);

    // Bring the loop down. Closes the listening socket — any in-flight
    // accept returns EBADF and the loop exits — then joins the thread.
    void stop();

    // Real bound port (useful when start was called with port == 0).
    int  bound_port() const;

private:
    int                    listen_fd_   = -1;
    int                    bound_port_  = 0;
    std::thread            loop_;
    std::atomic<bool>      running_{false};
    HttpHandler            handler_;

    void run_loop();
    void handle_client(int client_fd);
};

} // namespace budyk
