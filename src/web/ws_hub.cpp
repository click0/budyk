// SPDX-License-Identifier: BSD-3-Clause
#include "web/ws_hub.h"

#include "web/sha1_base64.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace budyk {

namespace {

constexpr const char kWsMagic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool ieq(const std::string& a, const char* b) {
    const size_t blen = std::strlen(b);
    if (a.size() != blen) return false;
    for (size_t i = 0; i < blen; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool icontains(const std::string& hay, const char* needle) {
    const size_t nlen = std::strlen(needle);
    if (nlen == 0) return true;
    for (size_t i = 0; i + nlen <= hay.size(); ++i) {
        size_t k = 0;
        while (k < nlen &&
               std::tolower(static_cast<unsigned char>(hay[i + k])) ==
               std::tolower(static_cast<unsigned char>(needle[k]))) {
            ++k;
        }
        if (k == nlen) return true;
    }
    return false;
}

ssize_t send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::send(fd, p + total, len - total, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

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
    if (!ieq(req.header("Upgrade"), "websocket"))       return false;
    if (!icontains(req.header("Connection"), "upgrade")) return false;
    if (req.header("Sec-WebSocket-Version") != "13")    return false;
    if (req.header("Sec-WebSocket-Key").empty())        return false;
    return true;
}

WebSocketHub::WebSocketHub() = default;
WebSocketHub::~WebSocketHub() { close_all(); }

void WebSocketHub::add(int fd) {
    std::lock_guard<std::mutex> g(mtx_);
    clients_.push_back(fd);
}

void WebSocketHub::broadcast(const std::string& payload) {
    const std::string frame = ws_text_frame(payload);
    std::lock_guard<std::mutex> g(mtx_);
    auto it = clients_.begin();
    while (it != clients_.end()) {
        if (send_all(*it, frame.data(), frame.size()) < 0) {
            ::close(*it);
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void WebSocketHub::close_all() {
    std::lock_guard<std::mutex> g(mtx_);
    for (int fd : clients_) ::close(fd);
    clients_.clear();
}

size_t WebSocketHub::size() const {
    std::lock_guard<std::mutex> g(mtx_);
    return clients_.size();
}

} // namespace budyk
