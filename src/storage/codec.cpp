// SPDX-License-Identifier: BSD-3-Clause
#include "storage/codec.h"
#include "core/endian.h"

#include "core/codec.h"

#include <cstring>

namespace budyk {

namespace {

// CRC-32C (Castagnoli), polynomial 0x1EDC6F41 reflected = 0x82F63B78.
// Table-driven, one byte per step: portable, no SSE4.2 dependency, and
// about eight times faster than going bit by bit, which matters because
// every record read (each probe of a query's binary search included)
// checks one.
constexpr uint32_t kCrc32cPolyReflected = 0x82F63B78U;

struct Crc32cTable {
    uint32_t v[256];
};

constexpr Crc32cTable make_crc32c_table() {
    Crc32cTable t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (kCrc32cPolyReflected & (0U - (c & 1U)));
        }
        t.v[i] = c;
    }
    return t;
}

constexpr Crc32cTable kCrc32cTable = make_crc32c_table();

} // namespace

uint32_t crc32c(const void* data, size_t len, uint32_t seed) {
    uint32_t c = ~seed;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        c = (c >> 8) ^ kCrc32cTable.v[(c ^ p[i]) & 0xFFU];
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
    *p++ = s.folded ? kRecordFolded : 0;            // flags
    uint8_t* crc_slot = p; p += 4;
    std::memset(crc_slot, 0, 4);

    size_t enc_len = 0;
    if (sample_encode(&s, p, cap - kRecordHeaderSize, &enc_len) != 0) return -3;
    if (enc_len != sample_max_encoded_size())                          return -4;

    uint32_t c = crc32c(base, 10);          // ts + level + flags
    c          = crc32c(p,   enc_len, c);   // payload
    le_put_u32(crc_slot, c);

    *out_len = total;
    return 0;
}

int record_decode(const void* buf, size_t len, Sample* out) {
    if (buf == nullptr || out == nullptr) return -1;
    if (len <= kRecordHeaderSize)          return -2;
    const size_t payload = len - kRecordHeaderSize;

    const auto* base   = static_cast<const uint8_t*>(buf);
    uint32_t    stored = le_get_u32(base + 10);

    uint32_t c = crc32c(base, 10);
    c          = crc32c(base + kRecordHeaderSize, payload, c);
    if (c != stored)                       return -3;

    if (sample_decode(base + kRecordHeaderSize, payload, out) != 0) return -4;

    // Record framing is the source of truth for ts / level / flags.
    out->timestamp_nanos = le_get_u64(base);
    uint8_t lv = base[8];
    if (lv < 1 || lv > kMaxLevelId)                  return -5;
    out->level  = static_cast<Level>(lv);
    out->folded = (base[9] & kRecordFolded) != 0;
    return 0;
}

} // namespace budyk
