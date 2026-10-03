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

// Broadcast hub. Holds the connected client fds, all non-blocking, and
// never waits on a client (spec §3.3 item 4: one broadcast, slow
// consumers are dropped). The collector tick calls broadcast() and
// service(); the HTTP thread calls add(). A client is evicted, and
// its fd closed, when:
//   - a send would block or writes only part of a frame (the socket
//     buffer is full: the peer has not read for a long time, or the
//     connection is half-open);
//   - it sends a close frame, or closes the connection;
//   - it sends bytes that are not a WebSocket frame.
class WebSocketHub {
public:
    WebSocketHub();
    ~WebSocketHub();

    // After a successful handshake, register the fd. The hub now owns
    // the fd — it is switched to non-blocking, and close() / close_all()
    // release it.
    void  add(int fd);

    // Send a text frame to every registered client. Clients whose
    // write fails or would block are removed and their fd closed.
    void  broadcast(const std::string& payload);

    // Read whatever the clients have sent and act on it: answer pings
    // with pongs, answer and honour close frames, drop clients that
    // hung up or talk garbage, skip over data frames (the dashboard
    // sends none). Returns the number of clients removed. Call it once
    // per tick, before size() is used for the client count, so a
    // vanished dashboard releases L3 within a tick.
    size_t service();

    // Drop every client; closes all owned fds.
    void  close_all();

    size_t size() const;

private:
    struct Client {
        int         fd;
        std::string inbuf;      // bytes received, not yet parsed as a frame
        uint64_t    skip;       // payload bytes of a data frame still to discard
    };

    // Parse and act on the frames in c->inbuf. Returns false when the
    // client must be dropped (close frame, or not a WebSocket frame).
    bool consume_frames(Client* c);

    mutable std::mutex  mtx_;
    std::vector<Client> clients_;
};

} // namespace budyk
