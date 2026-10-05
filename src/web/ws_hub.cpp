// SPDX-License-Identifier: BSD-3-Clause
#include "web/ws_hub.h"

#include "web/http_util.h"
#include "web/sha1_base64.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace budyk {

namespace {

constexpr const char kWsMagic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Write a whole frame to a non-blocking socket. Anything short of the
// whole frame — EAGAIN, a partial write, an error — returns -1 and the
// caller drops the client: a half-sent frame cannot be resumed later
// without corrupting the stream, and a full socket buffer means the
// peer has not read for a long time (frames are a few hundred bytes a
// second; the buffer holds minutes of them).
ssize_t send_frame(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::send(fd, p + total, len - total, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

// A control frame from the server: FIN=1, opcode, no mask, payload of
// at most 125 bytes (RFC 6455 §5.5).
std::string ws_control_frame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
    f.push_back(static_cast<char>(payload.size() & 0x7F));
    f.append(payload);
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

void WebSocketHub::add(int fd) {
    // Non-blocking from here on: broadcast() and service() must never
    // wait on a client. (The handshake was sent before add().)
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    std::lock_guard<std::mutex> g(mtx_);
    clients_.push_back(Client{fd, std::string(), 0});
}

void WebSocketHub::broadcast(const std::string& payload) {
    const std::string frame = ws_text_frame(payload);
    std::lock_guard<std::mutex> g(mtx_);
    auto it = clients_.begin();
    while (it != clients_.end()) {
        if (send_frame(it->fd, frame.data(), frame.size()) < 0) {
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
                send_frame(c->fd, reply.data(), reply.size());
                return false;
            }
            if (opcode == kOpPing) {
                const std::string pong = ws_control_frame(0xA, payload);
                if (send_frame(c->fd, pong.data(), pong.size()) < 0) return false;
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
        bool keep = true;
        char tmp[512];
        for (;;) {
            const ssize_t n = ::recv(it->fd, tmp, sizeof(tmp), MSG_DONTWAIT);
            if (n > 0) {
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
