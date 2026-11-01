// SPDX-License-Identifier: BSD-3-Clause
#include "web/ws_hub.h"

#include "web/http_util.h"
#include "web/sha1_base64.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>

namespace budyk {

namespace {

constexpr const char kWsMagic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// send(2) what a non-blocking socket takes now: the bytes written (0
// when it would block), -1 when the peer is gone or the socket failed.
// The caller keeps the rest for later (queue_out): a partial write used
// to drop the client, and a frame larger than a fresh socket's buffer
// (the first tick's detail block on a host with hundreds of interfaces)
// dropped the dashboard on every reconnect.
ssize_t send_some(int fd, const void* data, size_t len) {
    for (;;) {
        const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

// A control frame from the server: FIN=1, opcode, no mask, payload of
// at most 125 bytes (RFC 6455 §5.5).
std::string ws_control_frame(uint8_t opcode, const std::string& payload) {
    // A longer payload is cut to 125: `size & 0x7F` wrote a length that
    // did not match what followed (no caller passes one today).
    const size_t len = payload.size() < 125 ? payload.size() : 125;
    std::string f;
    f.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
    f.push_back(static_cast<char>(len));
    f.append(payload, 0, len);
    return f;
}

constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing  = 0x9;
// Unparsed input a client may accumulate before it is dropped: a frame
// header is at most 14 bytes and a control payload at most 125, so a
// well-behaved client never comes near this.
constexpr size_t  kMaxInbuf = 1024;

} // namespace

std::string ws_accept_key(const std::string& key) {
    std::string concat = key;
    concat.append(kWsMagic);
    const std::string digest = sha1(concat.data(), concat.size());
    return base64_encode(digest.data(), digest.size());
}

std::string ws_text_frame(const std::string& payload) {
    std::string f;
    f.reserve(payload.size() + 10);

    f.push_back(static_cast<char>(0x81));   // FIN=1, opcode=1 (text)

    const size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        f.push_back(static_cast<char>(126));
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>( n       & 0xFF));
    } else {
        f.push_back(static_cast<char>(127));
        const uint64_t n64 = static_cast<uint64_t>(n);
        for (int i = 7; i >= 0; --i) {
            f.push_back(static_cast<char>((n64 >> (i * 8)) & 0xFF));
        }
    }
    f.append(payload);
    return f;
}

std::string ws_handshake_response(const std::string& sec_websocket_key) {
    std::string accept = ws_accept_key(sec_websocket_key);
    std::string out;
    out.reserve(160);
    out.append("HTTP/1.1 101 Switching Protocols\r\n");
    out.append("Upgrade: websocket\r\n");
    out.append("Connection: Upgrade\r\n");
    out.append("Sec-WebSocket-Accept: ");
    out.append(accept);
    out.append("\r\n\r\n");
    return out;
}

bool is_websocket_upgrade(const HttpRequest& req) {
    if (req.method != "GET")                            return false;
    if (!ascii_ieq(req.header("Upgrade"), "websocket"))       return false;
    if (!ascii_icontains(req.header("Connection"), "upgrade")) return false;
    if (req.header("Sec-WebSocket-Version") != "13")    return false;
    if (req.header("Sec-WebSocket-Key").empty())        return false;
    return true;
}

WebSocketHub::WebSocketHub() = default;
WebSocketHub::~WebSocketHub() { close_all(); }

void WebSocketHub::add(int fd, std::string session) {
    // Non-blocking from here on: broadcast() and service() must never
    // wait on a client. (The handshake was sent before add().)
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    std::lock_guard<std::mutex> g(mtx_);
    clients_.push_back(Client{fd, std::string(), 0, std::move(session), std::string()});
}

size_t WebSocketHub::drop_sessions(const std::function<bool(const std::string&)>& valid) {
    std::vector<std::pair<int, std::string>> tokens;
    {
        std::lock_guard<std::mutex> g(mtx_);
        for (const auto& c : clients_) {
            if (!c.session.empty()) tokens.emplace_back(c.fd, c.session);
        }
    }
    std::vector<int> gone;
    for (const auto& t : tokens) {
        if (!valid(t.second)) gone.push_back(t.first);
    }
    if (gone.empty()) return 0;
    size_t dropped = 0;
    std::lock_guard<std::mutex> g(mtx_);
    for (auto it = clients_.begin(); it != clients_.end(); ) {
        if (std::find(gone.begin(), gone.end(), it->fd) != gone.end()) {
            // A close frame first (1008, policy violation), as far as the
            // socket takes it: the page can tell a session that ended
            // from a dropped connection.
            const std::string frame = ws_control_frame(kOpClose, std::string("\x03\xf0", 2));
            send_some(it->fd, frame.data(), frame.size());
            ::close(it->fd);
            it = clients_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

bool WebSocketHub::flush_out(Client* c) {
    while (!c->outbuf.empty()) {
        const ssize_t n = send_some(c->fd, c->outbuf.data(), c->outbuf.size());
        if (n < 0) return false;
        if (n == 0) break;                           // no room now: next time
        c->outbuf.erase(0, static_cast<size_t>(n));
    }
    return true;
}

bool WebSocketHub::queue_out(Client* c, const std::string& frame) {
    if (!flush_out(c)) return false;
    size_t sent = 0;
    if (c->outbuf.empty()) {                         // in order: nothing owed first
        const ssize_t n = send_some(c->fd, frame.data(), frame.size());
        if (n < 0) return false;
        sent = static_cast<size_t>(n);
    }
    if (sent < frame.size()) c->outbuf.append(frame, sent, std::string::npos);
    return c->outbuf.size() <= kMaxBacklog;
}

void WebSocketHub::broadcast(const std::string& payload) {
    const std::string frame = ws_text_frame(payload);
    std::lock_guard<std::mutex> g(mtx_);
    auto it = clients_.begin();
    while (it != clients_.end()) {
        if (!queue_out(&*it, frame)) {
            ::close(it->fd);
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

bool WebSocketHub::consume_frames(Client* c) {
    std::string& b = c->inbuf;
    for (;;) {
        // Finish discarding a data frame's payload first.
        if (c->skip > 0) {
            const size_t take = b.size() < c->skip ? b.size() : static_cast<size_t>(c->skip);
            b.erase(0, take);
            c->skip -= take;
            if (c->skip > 0) return true;        // need more bytes
        }
        if (b.size() < 2) return true;

        const uint8_t  b0     = static_cast<uint8_t>(b[0]);
        const uint8_t  b1     = static_cast<uint8_t>(b[1]);
        const uint8_t  opcode = b0 & 0x0F;
        const bool     masked = (b1 & 0x80) != 0;
        uint64_t       len    = b1 & 0x7F;
        size_t         hdr    = 2;

        // RSV bits must be zero, client frames must be masked (§5.1),
        // and the opcode must be one RFC 6455 defines. Anything else is
        // not a WebSocket peer.
        if ((b0 & 0x70) != 0 || !masked) return false;
        if (opcode > 0x2 && (opcode < 0x8 || opcode > 0xA)) return false;

        if (len == 126)      hdr += 2;
        else if (len == 127) hdr += 8;
        if (b.size() < hdr) return true;
        if (len == 126) {
            len = (static_cast<uint64_t>(static_cast<uint8_t>(b[2])) << 8) |
                   static_cast<uint64_t>(static_cast<uint8_t>(b[3]));
        } else if (len == 127) {
            len = 0;
            for (size_t i = 0; i < 8; ++i) {
                len = (len << 8) | static_cast<uint8_t>(b[2 + i]);
            }
        }
        const size_t mask_at = hdr;
        hdr += 4;
        if (b.size() < hdr) return true;

        if (opcode >= 0x8) {
            // Control frame: at most 125 bytes, never fragmented (§5.5).
            if (len > 125 || (b0 & 0x80) == 0) return false;
            if (b.size() < hdr + len) return true;
            std::string payload(b, hdr, static_cast<size_t>(len));
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(
                    static_cast<uint8_t>(payload[i]) ^
                    static_cast<uint8_t>(b[mask_at + (i % 4)]));
            }
            b.erase(0, hdr + static_cast<size_t>(len));
            if (opcode == kOpClose) {
                // Echo the status code (first two bytes), if any, and
                // let the peer see the close before the fd goes.
                const std::string reply = ws_control_frame(
                    kOpClose, payload.size() >= 2 ? payload.substr(0, 2) : std::string());
                send_some(c->fd, reply.data(), reply.size());
                return false;
            }
            if (opcode == kOpPing) {
                if (!queue_out(c, ws_control_frame(0xA, payload))) return false;
            }
            // A pong needs no answer.
            continue;
        }

        // Data frame: the hub has no use for client data. Drop the
        // header now and the payload as it arrives.
        b.erase(0, hdr);
        c->skip = len;
    }
}

size_t WebSocketHub::service() {
    std::lock_guard<std::mutex> g(mtx_);
    size_t dropped = 0;
    auto it = clients_.begin();
    while (it != clients_.end()) {
        bool   keep   = flush_out(&*it);             // what it is owed since the last tick
        size_t budget = kServiceReadBudget;
        char   tmp[512];
        while (keep && budget > 0) {
            const size_t  want = budget < sizeof(tmp) ? budget : sizeof(tmp);
            const ssize_t n    = ::recv(it->fd, tmp, want, MSG_DONTWAIT);
            if (n > 0) {
                budget -= static_cast<size_t>(n);
                it->inbuf.append(tmp, static_cast<size_t>(n));
                if (!consume_frames(&*it) || it->inbuf.size() > kMaxInbuf) {
                    keep = false;
                    break;
                }
                continue;
            }
            if (n == 0) { keep = false; break; }            // peer closed
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) keep = false;   // reset etc.
            break;
        }
        if (keep) {
            ++it;
        } else {
            ::close(it->fd);
            it = clients_.erase(it);
            ++dropped;
        }
    }
    return dropped;
}

void WebSocketHub::close_all() {
    std::lock_guard<std::mutex> g(mtx_);
    for (const auto& c : clients_) ::close(c.fd);
    clients_.clear();
}

size_t WebSocketHub::size() const {
    std::lock_guard<std::mutex> g(mtx_);
    return clients_.size();
}

} // namespace budyk
