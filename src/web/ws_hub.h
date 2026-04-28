// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "web/server.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace budyk {

// Build the response body for a WebSocket handshake (RFC 6455 §1.3).
// Given the value of the client's Sec-WebSocket-Key header, returns
// SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11") base64-encoded.
std::string ws_accept_key(const std::string& sec_websocket_key);

// Build the raw bytes of a server-to-client text frame: FIN=1, opcode=1,
// no MASK, length-encoded per RFC 6455 §5.2 (7-bit, 16-bit or 64-bit).
// Caller is responsible for newlines / framing of the JSON payload.
std::string ws_text_frame(const std::string& payload);

// Build the standard handshake response head (101 Switching Protocols
// + the three required headers). Returned string is ready to send().
std::string ws_handshake_response(const std::string& sec_websocket_key);

// Returns true when the request looks like a valid WebSocket upgrade
// (RFC 6455 §4.2.1): GET, Upgrade: websocket, Connection contains
// Upgrade, version 13, non-empty key.
bool is_websocket_upgrade(const HttpRequest& req);

// Broadcast hub. Stores connected client fds; sends each broadcast
// frame to all of them, lazily dropping any whose write fails.
class WebSocketHub {
public:
    WebSocketHub();
    ~WebSocketHub();

    // After a successful handshake, register the fd. The hub now owns
    // the fd — close() / close_all() will release it.
    void  add(int fd);

    // Send a text frame to every registered client. Clients whose
    // write fails are removed and their fd closed.
    void  broadcast(const std::string& payload);

    // Drop every client; closes all owned fds.
    void  close_all();

    size_t size() const;

private:
    mutable std::mutex mtx_;
    std::vector<int>   clients_;
};

} // namespace budyk
