// SPDX-License-Identifier: BSD-3-Clause
#include "storage/codec.h"
#include "core/endian.h"

#include "core/codec.h"

#include <cstring>

namespace budyk {

namespace {

// CRC-32C (Castagnoli), polynomial 0x1EDC6F41 reflected = 0x82F63B78.
// Per-byte software impl — small, portable, no SSE4.2 dependency.
constexpr uint32_t kCrc32cPolyReflected = 0x82F63B78U;

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

    le_put_u64(p, s.timestamp_nanos); p += 8;
    *p++ = static_cast<uint8_t>(s.level);
    *p++ = 0;                                       // pad
    uint8_t* crc_slot = p; p += 4;
    std::memset(crc_slot, 0, 4);

    size_t enc_len = 0;
    if (sample_encode(&s, p, cap - kRecordHeaderSize, &enc_len) != 0) return -3;
    if (enc_len != sample_max_encoded_size())                          return -4;

    uint32_t c = crc32c(base, 10);          // ts + level + pad
    c          = crc32c(p,   enc_len, c);   // payload
    le_put_u32(crc_slot, c);

    *out_len = total;
    return 0;
}

int record_decode(const void* buf, size_t len, Sample* out) {
    if (buf == nullptr || out == nullptr) return -1;
    const size_t total = record_size_for_sample();
    if (len < total)                       return -2;

    const auto* base   = static_cast<const uint8_t*>(buf);
    uint32_t    stored = le_get_u32(base + 10);

    uint32_t c = crc32c(base, 10);
    c          = crc32c(base + kRecordHeaderSize, sample_max_encoded_size(), c);
    if (c != stored)                       return -3;

    if (sample_decode(base + kRecordHeaderSize,
                      len - kRecordHeaderSize, out) != 0) return -4;

    // Record framing is the source of truth for ts / level.
    out->timestamp_nanos = le_get_u64(base);
    uint8_t lv = base[8];
    if (lv < 1 || lv > kMaxLevelId)                  return -5;
    out->level = static_cast<Level>(lv);
    return 0;
}

} // namespace budyk
