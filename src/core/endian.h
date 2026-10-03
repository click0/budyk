// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <cstring>

namespace budyk {

// Little-endian field access for the on-disk formats (the sample codec,
// the record framing, the ring file header). Byte order is fixed so a
// ring written on one host reads on another; memcpy keeps it free of
// alignment assumptions.

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

inline void     le_put_u32(uint8_t* p, uint32_t v) { const uint32_t le = to_le32(v); std::memcpy(p, &le, 4); }
inline void     le_put_u64(uint8_t* p, uint64_t v) { const uint64_t le = to_le64(v); std::memcpy(p, &le, 8); }
inline uint32_t le_get_u32(const uint8_t* p)       { uint32_t v; std::memcpy(&v, p, 4); return to_le32(v); }
inline uint64_t le_get_u64(const uint8_t* p)       { uint64_t v; std::memcpy(&v, p, 8); return to_le64(v); }

} // namespace budyk
