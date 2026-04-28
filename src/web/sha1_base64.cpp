// SPDX-License-Identifier: BSD-3-Clause
#include "web/sha1_base64.h"

#include <cstring>

namespace budyk {

namespace {

inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1_block(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] =  static_cast<uint32_t>(block[i * 4    ]) << 24 |
                static_cast<uint32_t>(block[i * 4 + 1]) << 16 |
                static_cast<uint32_t>(block[i * 4 + 2]) << 8  |
                static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if      (i < 20) { f = (b & c) | (~b & d);             k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                       k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

} // namespace

void sha1_init(Sha1* s) {
    s->state[0] = 0x67452301;
    s->state[1] = 0xEFCDAB89;
    s->state[2] = 0x98BADCFE;
    s->state[3] = 0x10325476;
    s->state[4] = 0xC3D2E1F0;
    s->bits   = 0;
    s->buflen = 0;
}

void sha1_update(Sha1* s, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    s->bits += static_cast<uint64_t>(len) * 8ULL;

    if (s->buflen > 0) {
        const size_t want = 64 - s->buflen;
        const size_t take = len < want ? len : want;
        std::memcpy(s->buf + s->buflen, p, take);
        s->buflen += take;
        p   += take;
        len -= take;
        if (s->buflen == 64) {
            sha1_block(s->state, s->buf);
            s->buflen = 0;
        }
    }
    while (len >= 64) {
        sha1_block(s->state, p);
        p   += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(s->buf, p, len);
        s->buflen = len;
    }
}

void sha1_final(Sha1* s, uint8_t out[20]) {
    uint8_t pad[64] = {0x80};
    const uint64_t total_bits = s->bits;

    const size_t pad_len = (s->buflen < 56) ? (56 - s->buflen)
                                            : (120 - s->buflen);
    sha1_update(s, pad, pad_len);

    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) {
        len_be[i] = static_cast<uint8_t>((total_bits >> (56 - i * 8)) & 0xFF);
    }
    sha1_update(s, len_be, 8);

    for (int i = 0; i < 5; ++i) {
        out[i * 4    ] = static_cast<uint8_t>((s->state[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((s->state[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((s->state[i] >>  8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>( s->state[i]        & 0xFF);
    }
}

std::string sha1(const void* data, size_t len) {
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    uint8_t out[20];
    sha1_final(&s, out);
    return std::string(reinterpret_cast<char*>(out), 20);
}

std::string base64_encode(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < len) {
        const uint32_t triple =
            (static_cast<uint32_t>(p[i    ]) << 16) |
            (static_cast<uint32_t>(p[i + 1]) <<  8) |
             static_cast<uint32_t>(p[i + 2]);
        out.push_back(kB64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back(kB64Alphabet[(triple >>  6) & 0x3F]);
        out.push_back(kB64Alphabet[ triple        & 0x3F]);
        i += 3;
    }
    if (i < len) {
        const uint32_t a = p[i];
        const uint32_t b = (i + 1 < len) ? p[i + 1] : 0;
        const uint32_t triple = (a << 16) | (b << 8);
        out.push_back(kB64Alphabet[(triple >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kB64Alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace budyk
