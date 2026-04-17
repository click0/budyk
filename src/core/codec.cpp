// SPDX-License-Identifier: BSD-3-Clause
#include "core/codec.h"

#include <cstdint>
#include <cstring>

namespace budyk {

namespace {

constexpr uint8_t  kMagic[8] = {'B', 'D', 'Y', 'K', 'S', 'M', 'P', 'L'};
constexpr uint32_t kVersion  = 1;

// Fixed little-endian layout, versioned. Sized to match current Sample struct.
constexpr size_t kEncodedSize =
      8 + 4 + 4     // magic, version, flags
    + 8 + 1 + 7     // timestamp_nanos, level, pad
    + 8 + 4 + 4     // cpu.total_percent, cpu.count, pad
    + 8 + 8 + 8     // mem.total, mem.available, mem.available_percent
    + 8 + 8 + 8     // swap.total, swap.used, swap.used_percent
    + 8 + 8 + 8     // load.avg_{1,5,15}m
    + 8;            // uptime_seconds

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

inline void put_u8 (uint8_t*& p, uint8_t  v)         { *p++ = v; }
inline void put_u32(uint8_t*& p, uint32_t v)         { uint32_t le = to_le32(v); std::memcpy(p, &le, 4); p += 4; }
inline void put_u64(uint8_t*& p, uint64_t v)         { uint64_t le = to_le64(v); std::memcpy(p, &le, 8); p += 8; }
inline void put_f64(uint8_t*& p, double   v)         { uint64_t u; std::memcpy(&u, &v, 8); put_u64(p, u); }
inline void put_pad(uint8_t*& p, size_t   n)         { std::memset(p, 0, n); p += n; }

inline uint8_t  get_u8 (const uint8_t*& p)           { return *p++; }
inline uint32_t get_u32(const uint8_t*& p)           { uint32_t v; std::memcpy(&v, p, 4); p += 4; return to_le32(v); }
inline uint64_t get_u64(const uint8_t*& p)           { uint64_t v; std::memcpy(&v, p, 8); p += 8; return to_le64(v); }
inline double   get_f64(const uint8_t*& p)           { uint64_t u = get_u64(p); double d; std::memcpy(&d, &u, 8); return d; }
inline void     skip   (const uint8_t*& p, size_t n) { p += n; }

} // namespace

size_t sample_max_encoded_size() { return kEncodedSize; }

int sample_encode(const Sample* s, void* buf, size_t cap, size_t* out_len) {
    if (s == nullptr || buf == nullptr || out_len == nullptr) return -1;
    if (cap < kEncodedSize) return -2;

    auto* p = static_cast<uint8_t*>(buf);
    std::memcpy(p, kMagic, 8); p += 8;
    put_u32(p, kVersion);
    put_u32(p, 0);

    put_u64(p, s->timestamp_nanos);
    put_u8 (p, static_cast<uint8_t>(s->level));
    put_pad(p, 7);

    put_f64(p, s->cpu.total_percent);
    put_u32(p, s->cpu.count);
    put_pad(p, 4);

    put_u64(p, s->mem.total);
    put_u64(p, s->mem.available);
    put_f64(p, s->mem.available_percent);

    put_u64(p, s->swap.total);
    put_u64(p, s->swap.used);
    put_f64(p, s->swap.used_percent);

    put_f64(p, s->load.avg_1m);
    put_f64(p, s->load.avg_5m);
    put_f64(p, s->load.avg_15m);

    put_f64(p, s->uptime_seconds);

    *out_len = kEncodedSize;
    return 0;
}

int sample_decode(const void* buf, size_t len, Sample* out) {
    if (buf == nullptr || out == nullptr) return -1;
    if (len < kEncodedSize) return -2;

    const auto* p = static_cast<const uint8_t*>(buf);
    if (std::memcmp(p, kMagic, 8) != 0) return -3;
    p += 8;

    uint32_t version = get_u32(p);
    if (version != kVersion) return -4;
    (void)get_u32(p);

    out->timestamp_nanos = get_u64(p);
    uint8_t lv = get_u8(p);
    if (lv < 1 || lv > 3) return -5;
    out->level = static_cast<Level>(lv);
    skip(p, 7);

    out->cpu.total_percent = get_f64(p);
    out->cpu.count         = get_u32(p);
    skip(p, 4);

    out->mem.total             = get_u64(p);
    out->mem.available         = get_u64(p);
    out->mem.available_percent = get_f64(p);

    out->swap.total        = get_u64(p);
    out->swap.used         = get_u64(p);
    out->swap.used_percent = get_f64(p);

    out->load.avg_1m  = get_f64(p);
    out->load.avg_5m  = get_f64(p);
    out->load.avg_15m = get_f64(p);

    out->uptime_seconds = get_f64(p);
    return 0;
}

} // namespace budyk
