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
//   +28  u64    write_idx   <-- atomically updated
//   +36  u8[28] reserved
constexpr size_t   kHeaderSize      = 64;
constexpr uint32_t kFormatVersion   = 1;
constexpr char     kMagicBytes[8]   = {'B', 'D', 'Y', 'K', 'R', 'B', '\0', '\x01'};
constexpr size_t   kWriteIdxOffset  = 28;

} // namespace

int RingFile::open(const char* path, uint8_t tier, uint32_t record_size, uint64_t capacity) {
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
    int fd = ::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)                      { last_errno_ = errno; return -3; }

    struct stat st{};
    if (::fstat(fd, &st) != 0)       { last_errno_ = errno; ::close(fd); return -4; }
    const bool fresh = (st.st_size == 0);

    if (fresh) {
        if (::ftruncate(fd, static_cast<off_t>(file_bytes)) != 0) {
            last_errno_ = errno; ::close(fd); return -5;
        }
    } else if (static_cast<size_t>(st.st_size) != file_bytes) {
        ::close(fd); return -6;
    }

    void* m = ::mmap(nullptr, kHeaderSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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

uint64_t RingFile::capacity() const { return capacity_; }

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
        default:  return "unknown error";
    }
}

} // namespace budyk
