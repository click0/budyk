// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "web/server.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace budyk {

/// Build the response body for a WebSocket handshake (RFC 6455 §1.3).
/// Given the value of the client's Sec-WebSocket-Key header, returns
/// SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11") base64-encoded.
std::string ws_accept_key(const std::string& sec_websocket_key);

/// Build the raw bytes of a server-to-client text frame: FIN=1, opcode=1,
/// no MASK, length-encoded per RFC 6455 §5.2 (7-bit, 16-bit or 64-bit).
/// Caller is responsible for newlines / framing of the JSON payload.
std::string ws_text_frame(const std::string& payload);

/// Build the standard handshake response head (101 Switching Protocols
/// + the three required headers). Returned string is ready to send().
std::string ws_handshake_response(const std::string& sec_websocket_key);

/// Returns true when the request looks like a valid WebSocket upgrade
/// (RFC 6455 §4.2.1): GET, Upgrade: websocket, Connection contains
/// Upgrade, version 13, non-empty key.
bool is_websocket_upgrade(const HttpRequest& req);

/// Broadcast hub. Holds the connected client fds, all non-blocking, and
/// never waits on a client (spec §3.3 item 4: one broadcast, slow
/// consumers are dropped). The collector tick calls broadcast() and
/// service(); the HTTP thread calls add(). What a client's socket does
/// not take at once is kept and sent as the peer reads (a frame larger
/// than a fresh socket's buffer was a partial write and a drop: a
/// dashboard's first frame on a host with hundreds of interfaces, on
/// every reconnect). A client is evicted, and its fd closed, when:
///   - it is more than kMaxBacklog behind (the peer has not read for a
///     long time, or the connection is half-open);
///   - it sends a close frame, or closes the connection;
///   - it sends bytes that are not a WebSocket frame.
class WebSocketHub {
public:
    WebSocketHub();
    ~WebSocketHub();

    /// At most this many clients: the upgrade is refused past it (503),
    /// so WebSocket connections alone cannot use up the daemon's
    /// descriptors.
    static constexpr size_t kMaxClients = 64;
    /// Unsent bytes a client may fall behind by before it is dropped:
    /// minutes of frames for a dashboard, a few of the largest.
    static constexpr size_t kMaxBacklog = 1024 * 1024;
    /// Bytes service() reads from one client per call. A client that kept
    /// sending (a data frame of any length, which the hub discards) held
    /// the collector tick for as long as it sent; the rest waits in the
    /// socket, where TCP holds the sender back.
    static constexpr size_t kServiceReadBudget = 4096;

    /// After a successful handshake, register the fd. The hub now owns
    /// the fd — it is switched to non-blocking, and close() / close_all()
    /// release it. `session`: the session token the upgrade was
    /// authenticated with ("" with auth off), for drop_sessions().
    void  add(int fd, std::string session = std::string());
    /// Drop every client whose session token `valid` no longer accepts
    /// (logged out, expired). Clients without a token stay. `valid` runs
    /// without the hub's lock. Returns the number dropped.
    size_t drop_sessions(const std::function<bool(const std::string&)>& valid);

    /// Send a text frame to every registered client, after what it still
    /// owes them. A client whose socket failed, or that is more than
    /// kMaxBacklog behind, is removed and its fd closed.
    void  broadcast(const std::string& payload);

    /// Send what the clients are owed, read what they have sent (at
    /// most kServiceReadBudget each) and act on it: answer pings with
    /// pongs, answer and honour close frames, drop clients that hung up
    /// or talk garbage, skip over data frames (the dashboard sends
    /// none). Returns the number of clients removed. Call it once per
    /// tick, before size() is used for the client count, so a vanished
    /// dashboard releases L3 within a tick.
    size_t service();

    /// Drop every client; closes all owned fds.
    void  close_all();

    size_t size() const;

private:
    struct Client {
        int         fd;
        std::string inbuf;      ///< bytes received, not yet parsed as a frame
        uint64_t    skip;       ///< payload bytes of a data frame still to discard
        std::string session;    ///< the token it was let in with ("" with auth off)
        std::string outbuf;     ///< frames' bytes the socket has not taken yet
    };

    /// Parse and act on the frames in c->inbuf. Returns false when the
    /// client must be dropped (close frame, or not a WebSocket frame).
    bool consume_frames(Client* c);
    /// Send what c->outbuf holds, as far as the socket takes it. False
    /// when the socket failed.
    bool flush_out(Client* c);
    /// flush_out(), then `frame`, the rest kept in c->outbuf. False when
    /// the socket failed or the client is more than kMaxBacklog behind.
    bool queue_out(Client* c, const std::string& frame);

    mutable std::mutex  mtx_;
    std::vector<Client> clients_;
};

} // namespace budyk
