// SPDX-License-Identifier: BSD-3-Clause
#include "storage/ring_file.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace budyk {

namespace {

// File header layout (64 bytes total, spec §3.5):
//   +0   u8[8]  magic  "BDYKRB\0\x01"
//   +8   u32    version
//   +12  u8     tier (1|2|3)
//   +13  u8[3]  pad
//   +16  u32    record_size
//   +20  u64    capacity
//   +28  u64    write_idx   <-- atomically updated
//   +36  u8[28] reserved
constexpr size_t   kHeaderSize      = 64;
constexpr uint32_t kFormatVersion   = 1;
constexpr char     kMagicBytes[8]   = {'B', 'D', 'Y', 'K', 'R', 'B', '\0', '\x01'};
constexpr size_t   kWriteIdxOffset  = 28;

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

inline void put_u32(uint8_t* p, uint32_t v) { uint32_t le = to_le32(v); std::memcpy(p, &le, 4); }
inline void put_u64(uint8_t* p, uint64_t v) { uint64_t le = to_le64(v); std::memcpy(p, &le, 8); }
inline uint32_t get_u32(const uint8_t* p)   { uint32_t v; std::memcpy(&v, p, 4); return to_le32(v); }
inline uint64_t get_u64(const uint8_t* p)   { uint64_t v; std::memcpy(&v, p, 8); return to_le64(v); }

} // namespace

int RingFile::open(const char* path, uint8_t tier, uint32_t record_size, uint64_t capacity) {
    if (fd_ >= 0)                    return -1;  // already open
    if (record_size == 0 || capacity == 0) return -2;

    const size_t file_bytes = kHeaderSize + static_cast<size_t>(record_size) * capacity;

    int fd = ::open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)                      return -3;

    struct stat st{};
    if (::fstat(fd, &st) != 0)       { ::close(fd); return -4; }
    const bool fresh = (st.st_size == 0);

    if (fresh) {
        if (::ftruncate(fd, static_cast<off_t>(file_bytes)) != 0) {
            ::close(fd); return -5;
        }
    } else if (static_cast<size_t>(st.st_size) != file_bytes) {
        ::close(fd); return -6;
    }

    void* m = ::mmap(nullptr, kHeaderSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)             { ::close(fd); return -7; }

    auto* h = static_cast<uint8_t*>(m);
    if (fresh) {
        std::memset(h, 0, kHeaderSize);
        std::memcpy(h, kMagicBytes, 8);
        put_u32(h + 8,  kFormatVersion);
        h[12] = tier;
        put_u32(h + 16, record_size);
        put_u64(h + 20, capacity);
        put_u64(h + kWriteIdxOffset, 0);
    } else {
        if (std::memcmp(h, kMagicBytes, 8) != 0) { ::munmap(m, kHeaderSize); ::close(fd); return -8; }
        if (get_u32(h + 8)   != kFormatVersion)  { ::munmap(m, kHeaderSize); ::close(fd); return -9; }
        if (h[12]            != tier)            { ::munmap(m, kHeaderSize); ::close(fd); return -10; }
        if (get_u32(h + 16)  != record_size)     { ::munmap(m, kHeaderSize); ::close(fd); return -11; }
        if (get_u64(h + 20)  != capacity)        { ::munmap(m, kHeaderSize); ::close(fd); return -12; }
    }

    fd_          = fd;
    mmap_base_   = m;
    mmap_len_    = kHeaderSize;
    record_size_ = record_size;
    capacity_    = capacity;
    return 0;
}

void RingFile::close() {
    if (mmap_base_ != nullptr) {
        ::munmap(mmap_base_, mmap_len_);
        mmap_base_ = nullptr;
        mmap_len_  = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    record_size_ = 0;
    capacity_    = 0;
}

int RingFile::append(const void* record, size_t len) {
    if (fd_ < 0 || record == nullptr)          return -1;
    if (len != record_size_)                   return -2;

    auto*     widx_ptr = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(mmap_base_) + kWriteIdxOffset);
    uint64_t  idx      = __atomic_load_n(widx_ptr, __ATOMIC_ACQUIRE);
    uint64_t  slot     = idx % capacity_;
    off_t     off      = static_cast<off_t>(kHeaderSize + slot * record_size_);

    ssize_t w = ::pwrite(fd_, record, record_size_, off);
    if (w != static_cast<ssize_t>(record_size_)) return -3;

    // Advance write_idx only after the record is durably placed. A crash
    // between pwrite and this increment loses at most the last record.
    __atomic_fetch_add(widx_ptr, 1, __ATOMIC_RELEASE);
    return 0;
}

int RingFile::read_at(uint64_t index, void* out, size_t len) const {
    if (fd_ < 0 || out == nullptr)             return -1;
    if (len != record_size_)                   return -2;
    if (index >= capacity_)                    return -3;

    off_t off = static_cast<off_t>(kHeaderSize + index * record_size_);
    ssize_t r = ::pread(fd_, out, record_size_, off);
    if (r != static_cast<ssize_t>(record_size_)) return -4;
    return 0;
}

uint64_t RingFile::write_index() const {
    if (fd_ < 0) return 0;
    auto* widx_ptr = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(mmap_base_) + kWriteIdxOffset);
    return __atomic_load_n(widx_ptr, __ATOMIC_ACQUIRE);
}

uint64_t RingFile::count() const {
    const uint64_t idx = write_index();
    return idx < capacity_ ? idx : capacity_;
}

} // namespace budyk
