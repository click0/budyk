// SPDX-License-Identifier: BSD-3-Clause
#include "web/server.h"
#include "web/sha1_base64.h"
#include "web/ws_hub.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

using namespace budyk;

static std::string hex(const std::string& bin) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(bin.size() * 2);
    for (unsigned char b : bin) {
        out.push_back(d[(b >> 4) & 0xF]);
        out.push_back(d[ b       & 0xF]);
    }
    return out;
}

int main() {
    // 1. SHA-1 — RFC 3174 / NIST FIPS-180 vectors.
    {
        // "abc"
        assert(hex(sha1("abc", 3)) == "a9993e364706816aba3e25717850c26c9cd0d89d");
        // empty
        assert(hex(sha1("", 0))    == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        // "The quick brown fox jumps over the lazy dog"
        const char* s = "The quick brown fox jumps over the lazy dog";
        assert(hex(sha1(s, std::strlen(s))) ==
               "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
        // 56-byte boundary triggers the second block:
        // "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
        const char* sp = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        assert(hex(sha1(sp, std::strlen(sp))) ==
               "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    }

    // 2. base64 — RFC 4648 vectors.
    {
        assert(base64_encode("",      0)  == "");
        assert(base64_encode("f",     1)  == "Zg==");
        assert(base64_encode("fo",    2)  == "Zm8=");
        assert(base64_encode("foo",   3)  == "Zm9v");
        assert(base64_encode("foob",  4)  == "Zm9vYg==");
        assert(base64_encode("fooba", 5)  == "Zm9vYmE=");
        assert(base64_encode("foobar",6)  == "Zm9vYmFy");
    }

    // 3. ws_accept_key — RFC 6455 §1.3 worked example: the client key
    //    "dGhlIHNhbXBsZSBub25jZQ==" must yield exactly
    //    "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
    {
        assert(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") ==
               "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    }

    // 4. ws_handshake_response — 101 Switching Protocols + Accept hash.
    {
        const std::string resp = ws_handshake_response("dGhlIHNhbXBsZSBub25jZQ==");
        assert(resp.find("HTTP/1.1 101 Switching Protocols\r\n") == 0);
        assert(resp.find("Upgrade: websocket\r\n")               != std::string::npos);
        assert(resp.find("Connection: Upgrade\r\n")              != std::string::npos);
        assert(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") !=
               std::string::npos);
        assert(resp.size() >= 4 &&
               resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0);
    }

    // 5. ws_text_frame — three length-encoding regimes.
    {
        // 7-bit length: payload < 126 bytes
        const std::string p1(5, 'a');
        const std::string f1 = ws_text_frame(p1);
        assert(f1.size() == 2 + 5);
        assert(static_cast<unsigned char>(f1[0]) == 0x81);
        assert(static_cast<unsigned char>(f1[1]) == 5);
        assert(f1.compare(2, 5, p1) == 0);

        // 16-bit length: payload 200 bytes -> length byte 126, then 0x00 0xC8
        const std::string p2(200, 'b');
        const std::string f2 = ws_text_frame(p2);
        assert(f2.size() == 2 + 2 + 200);
        assert(static_cast<unsigned char>(f2[0]) == 0x81);
        assert(static_cast<unsigned char>(f2[1]) == 126);
        assert(static_cast<unsigned char>(f2[2]) == 0x00);
        assert(static_cast<unsigned char>(f2[3]) == 0xC8);

        // 64-bit length: payload 70_000 bytes -> length byte 127, then 8 bytes BE
        const size_t big = 70000;
        const std::string p3(big, 'c');
        const std::string f3 = ws_text_frame(p3);
        assert(f3.size() == 2 + 8 + big);
        assert(static_cast<unsigned char>(f3[0]) == 0x81);
        assert(static_cast<unsigned char>(f3[1]) == 127);
        // Big-endian 64-bit length, expect 0x00 0x00 0x00 0x00 0x00 0x01 0x11 0x70
        assert(static_cast<unsigned char>(f3[2 + 5]) == 0x01);
        assert(static_cast<unsigned char>(f3[2 + 6]) == 0x11);
        assert(static_cast<unsigned char>(f3[2 + 7]) == 0x70);
    }

    // 6. is_websocket_upgrade — only accepts a fully-formed upgrade request.
    {
        HttpRequest req;
        req.method = "GET";
        req.headers.push_back({"Upgrade",                "websocket"});
        req.headers.push_back({"Connection",             "keep-alive, Upgrade"});
        req.headers.push_back({"Sec-WebSocket-Version",  "13"});
        req.headers.push_back({"Sec-WebSocket-Key",      "dGhlIHNhbXBsZSBub25jZQ=="});
        assert(is_websocket_upgrade(req));

        // Missing Connection: Upgrade
        HttpRequest bad1 = req;
        bad1.headers.clear();
        bad1.headers.push_back({"Upgrade",               "websocket"});
        bad1.headers.push_back({"Sec-WebSocket-Version", "13"});
        bad1.headers.push_back({"Sec-WebSocket-Key",     "x"});
        assert(!is_websocket_upgrade(bad1));

        // Wrong version
        HttpRequest bad2 = req;
        for (auto& kv : bad2.headers) {
            if (kv.first == "Sec-WebSocket-Version") kv.second = "8";
        }
        assert(!is_websocket_upgrade(bad2));

        // Wrong method
        HttpRequest bad3 = req;
        bad3.method = "POST";
        assert(!is_websocket_upgrade(bad3));

        // Empty key
        HttpRequest bad4 = req;
        for (auto& kv : bad4.headers) {
            if (kv.first == "Sec-WebSocket-Key") kv.second = "";
        }
        assert(!is_websocket_upgrade(bad4));
    }

    // 7. WebSocketHub bookkeeping — add / size / close_all release fds.
    //    No real network here; we use a socketpair so the fds are
    //    valid and close_all can close them safely.
    {
        WebSocketHub hub;
        assert(hub.size() == 0);

        int sp1[2], sp2[2];
        assert(::pipe(sp1) == 0);
        assert(::pipe(sp2) == 0);
        hub.add(sp1[0]);
        hub.add(sp2[0]);
        assert(hub.size() == 2);

        hub.close_all();
        assert(hub.size() == 0);
        // Write ends are still ours; close them.
        ::close(sp1[1]);
        ::close(sp2[1]);
    }

    std::printf("test_ws: PASS\n");
    return 0;
}
