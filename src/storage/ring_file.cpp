// SPDX-License-Identifier: BSD-3-Clause
#include "storage/ring_file.h"
#include "core/endian.h"

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
//   +12  u8     tier (1|2|3; 0x80 for custom-level rings)
//   +13  u8[3]  pad
//   +16  u32    record_size
//   +20  u64    capacity
//   +28  u64    write_idx   records written; not 8-byte aligned, so it is
//                           copied in and out (widx_ is the atomic)
//   +36  u8[28] reserved
constexpr size_t   kHeaderSize      = 64;
constexpr uint32_t kFormatVersion   = 1;
constexpr char     kMagicBytes[8]   = {'B', 'D', 'Y', 'K', 'R', 'B', '\0', '\x01'};
constexpr size_t   kWriteIdxOffset  = 28;

} // namespace

int RingFile::open(const char* path, uint8_t tier, uint32_t record_size, uint64_t capacity) {
    return open_impl(path, tier, record_size, capacity, false);
}

int RingFile::open_read_only(const char* path, uint8_t tier, uint32_t record_size,
                             uint64_t capacity) {
    return open_impl(path, tier, record_size, capacity, true);
}

int RingFile::open_impl(const char* path, uint8_t tier, uint32_t record_size,
                        uint64_t capacity, bool read_only) {
    last_errno_ = 0;
    if (fd_ >= 0)                    return -1;  // already open
    if (record_size == 0 || capacity == 0) return -2;

    const size_t file_bytes = kHeaderSize + static_cast<size_t>(record_size) * capacity;

    // CLOEXEC: a child forked by rule exec() or an alert channel must
    // not hold the ring open — it could outlive the daemon and write
    // into a ring the next instance has already reopened.
    // 0600: the history (load, memory, process counts, file-watch events)
    // is the daemon's, like sessions.tsv; readers go through the API.
    // An existing file keeps its mode.
    int fd = read_only ? ::open(path, O_RDONLY | O_CLOEXEC)
                       : ::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)                      { last_errno_ = errno; return -3; }

    struct stat st{};
    if (::fstat(fd, &st) != 0)       { last_errno_ = errno; ::close(fd); return -4; }
    const bool fresh = (st.st_size == 0);
    if (fresh && read_only)          { ::close(fd); return -6; }

    if (fresh) {
        if (::ftruncate(fd, static_cast<off_t>(file_bytes)) != 0) {
            last_errno_ = errno; ::close(fd); return -5;
        }
    } else if (static_cast<size_t>(st.st_size) != file_bytes) {
        ::close(fd); return -6;
    }

    void* m = ::mmap(nullptr, kHeaderSize, read_only ? PROT_READ : PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)             { last_errno_ = errno; ::close(fd); return -7; }

    auto* h = static_cast<uint8_t*>(m);
    if (fresh) {
        std::memset(h, 0, kHeaderSize);
        std::memcpy(h, kMagicBytes, 8);
        le_put_u32(h + 8,  kFormatVersion);
        h[12] = tier;
        le_put_u32(h + 16, record_size);
        le_put_u64(h + 20, capacity);
        le_put_u64(h + kWriteIdxOffset, 0);
    } else {
        if (std::memcmp(h, kMagicBytes, 8) != 0) { ::munmap(m, kHeaderSize); ::close(fd); return -8; }
        if (le_get_u32(h + 8)   != kFormatVersion)  { ::munmap(m, kHeaderSize); ::close(fd); return -9; }
        if (h[12]            != tier)            { ::munmap(m, kHeaderSize); ::close(fd); return -10; }
        if (le_get_u32(h + 16)  != record_size)     { ::munmap(m, kHeaderSize); ::close(fd); return -11; }
        if (le_get_u64(h + 20)  != capacity)        { ::munmap(m, kHeaderSize); ::close(fd); return -12; }
    }

    fd_          = fd;
    read_only_   = read_only;
    tier_        = tier;
    mmap_base_   = m;
    mmap_len_    = kHeaderSize;
    record_size_ = record_size;
    capacity_    = capacity;
    widx_.store(le_get_u64(h + kWriteIdxOffset), std::memory_order_release);
    return 0;
}

int RingFile::read_header(const char* path, Header* out, int* err) {
    if (err != nullptr) *err = 0;
    if (path == nullptr || out == nullptr) return -2;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (err != nullptr) *err = errno; return -3; }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        if (err != nullptr) *err = errno;
        ::close(fd);
        return -4;
    }
    uint8_t h[kHeaderSize];
    const ssize_t n = st.st_size >= static_cast<off_t>(kHeaderSize)
                    ? ::pread(fd, h, kHeaderSize, 0) : 0;
    ::close(fd);
    if (n != static_cast<ssize_t>(kHeaderSize))       return -13;
    if (std::memcmp(h, kMagicBytes, 8) != 0)          return -8;
    if (le_get_u32(h + 8) != kFormatVersion)          return -9;
    out->tier        = h[12];
    out->record_size = le_get_u32(h + 16);
    out->capacity    = le_get_u64(h + 20);
    out->write_idx   = le_get_u64(h + kWriteIdxOffset);
    if (out->record_size == 0 || out->capacity == 0 ||
        out->capacity > (static_cast<uint64_t>(st.st_size) - kHeaderSize) / out->record_size ||
        static_cast<uint64_t>(st.st_size) !=
            kHeaderSize + static_cast<uint64_t>(out->record_size) * out->capacity) {
        return -6;
    }
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
    read_only_   = false;
    tier_        = 0;
    widx_.store(0, std::memory_order_release);
}

int RingFile::append(const void* record, size_t len) {
    if (fd_ < 0 || read_only_ || record == nullptr) return -1;
    if (len != record_size_)                   return -2;

    const uint64_t idx  = widx_.load(std::memory_order_acquire);
    const uint64_t slot = idx % capacity_;
    const off_t    off  = static_cast<off_t>(kHeaderSize + slot * record_size_);

    ssize_t w = ::pwrite(fd_, record, record_size_, off);
    if (w != static_cast<ssize_t>(record_size_)) return -3;

    // Advance write_idx only after the record is durably placed. A crash
    // between pwrite and this increment loses at most the last record.
    le_put_u64(static_cast<uint8_t*>(mmap_base_) + kWriteIdxOffset, idx + 1);
    widx_.store(idx + 1, std::memory_order_release);
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

int RingFile::advance_write_index(uint64_t n) {
    if (fd_ < 0 || read_only_) return -1;
    const uint64_t idx = widx_.load(std::memory_order_acquire) + n;
    le_put_u64(static_cast<uint8_t*>(mmap_base_) + kWriteIdxOffset, idx);
    widx_.store(idx, std::memory_order_release);
    return 0;
}

uint64_t RingFile::write_index() const {
    if (fd_ < 0) return 0;
    // Read-only (fsck, suggest-rules): the header as it is now, since a
    // running daemon may be writing the ring.
    if (read_only_) {
        return le_get_u64(static_cast<const uint8_t*>(mmap_base_) + kWriteIdxOffset);
    }
    return widx_.load(std::memory_order_acquire);
}

uint64_t RingFile::count() const {
    const uint64_t idx = write_index();
    return idx < capacity_ ? idx : capacity_;
}

uint64_t RingFile::capacity() const { return capacity_; }

uint8_t RingFile::tier() const { return tier_; }

int RingFile::last_errno() const { return last_errno_; }

const char* RingFile::describe(int rc) {
    switch (rc) {
        case 0:   return "ok";
        case -1:  return "already open";
        case -2:  return "record size or capacity is zero";
        case -3:  return "open failed";
        case -4:  return "fstat failed";
        case -5:  return "ftruncate failed";
        case -6:  return "file size does not match record size x capacity";
        case -7:  return "mmap failed";
        case -8:  return "not a budyk ring file (bad magic)";
        case -9:  return "ring format version mismatch";
        case -10: return "ring was written for another tier";
        case -11: return "record size in the header differs";
        case -12: return "capacity in the header differs";
        case -13: return "file is shorter than a ring header";
        default:  return "unknown error";
    }
}

} // namespace budyk
