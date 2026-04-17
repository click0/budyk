// SPDX-License-Identifier: BSD-3-Clause
#include "storage/codec.h"

#include "core/codec.h"

#include <cstring>

namespace budyk {

namespace {

// CRC-32C (Castagnoli), polynomial 0x1EDC6F41 reflected = 0x82F63B78.
// Per-byte software impl — small, portable, no SSE4.2 dependency.
constexpr uint32_t kCrc32cPolyReflected = 0x82F63B78U;

inline uint32_t to_le32(uint32_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}
inline uint64_t to_le64(uint64_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap64(v);
#else
    return v;
#endif
}

inline void     put_u64_le(uint8_t* p, uint64_t v) { uint64_t le = to_le64(v); std::memcpy(p, &le, 8); }
inline uint64_t get_u64_le(const uint8_t* p)       { uint64_t v; std::memcpy(&v, p, 8); return to_le64(v); }
inline void     put_u32_le(uint8_t* p, uint32_t v) { uint32_t le = to_le32(v); std::memcpy(p, &le, 4); }
inline uint32_t get_u32_le(const uint8_t* p)       { uint32_t v; std::memcpy(&v, p, 4); return to_le32(v); }

} // namespace

uint32_t crc32c(const void* data, size_t len, uint32_t seed) {
    uint32_t c = ~seed;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (kCrc32cPolyReflected & -(c & 1));
        }
    }
    return ~c;
}

size_t record_size_for_sample() {
    return kRecordHeaderSize + sample_max_encoded_size();
}

int record_encode(const Sample& s, void* buf, size_t cap, size_t* out_len) {
    if (buf == nullptr || out_len == nullptr)      return -1;
    const size_t total = record_size_for_sample();
    if (cap < total)                                return -2;

    auto*    base     = static_cast<uint8_t*>(buf);
    uint8_t* p        = base;

    put_u64_le(p, s.timestamp_nanos); p += 8;
    *p++ = static_cast<uint8_t>(s.level);
    *p++ = 0;                                       // pad
    uint8_t* crc_slot = p; p += 4;
    std::memset(crc_slot, 0, 4);

    size_t enc_len = 0;
    if (sample_encode(&s, p, cap - kRecordHeaderSize, &enc_len) != 0) return -3;
    if (enc_len != sample_max_encoded_size())                          return -4;

    uint32_t c = crc32c(base, 10);          // ts + level + pad
    c          = crc32c(p,   enc_len, c);   // payload
    put_u32_le(crc_slot, c);

    *out_len = total;
    return 0;
}

int record_decode(const void* buf, size_t len, Sample* out) {
    if (buf == nullptr || out == nullptr) return -1;
    const size_t total = record_size_for_sample();
    if (len < total)                       return -2;

    const auto* base   = static_cast<const uint8_t*>(buf);
    uint32_t    stored = get_u32_le(base + 10);

    uint32_t c = crc32c(base, 10);
    c          = crc32c(base + kRecordHeaderSize, sample_max_encoded_size(), c);
    if (c != stored)                       return -3;

    if (sample_decode(base + kRecordHeaderSize,
                      len - kRecordHeaderSize, out) != 0) return -4;

    // Record framing is the source of truth for ts / level.
    out->timestamp_nanos = get_u64_le(base);
    uint8_t lv = base[8];
    if (lv < 1 || lv > 3)                  return -5;
    out->level = static_cast<Level>(lv);
    return 0;
}

} // namespace budyk
