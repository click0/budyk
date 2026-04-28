// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace budyk {

// Pure-C SHA-1 (RFC 3174). Used by the WebSocket handshake; not for
// password hashing — that path goes through libargon2.
struct Sha1 {
    uint32_t state[5];
    uint64_t bits;
    uint8_t  buf[64];
    size_t   buflen;
};

void sha1_init  (Sha1* s);
void sha1_update(Sha1* s, const void* data, size_t len);
void sha1_final (Sha1* s, uint8_t out[20]);

// Convenience — feed once, return the 20-byte digest as a std::string.
std::string sha1(const void* data, size_t len);

// Standard alphabet, no line breaks. Used for the WS handshake's
// Sec-WebSocket-Accept response header.
std::string base64_encode(const void* data, size_t len);

} // namespace budyk
