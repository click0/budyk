// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <cstddef>

namespace budyk {

/// Header tier byte of a custom-level ring (the built-ins use 1..3).
constexpr uint8_t kCustomRingTier = 0x80;

/// On-disk ring-buffer file with fixed-size records.
/// Header: 64 bytes (magic, version, tier, record_size, capacity, write_idx).
/// Records: [0..capacity-1], each record_size bytes.
class RingFile {
public:
    int  open(const char* path, uint8_t tier, uint32_t record_size, uint64_t capacity);
    /// An existing ring, for reading only (fsck, suggest-rules): nothing
    /// is created or written, the header is mapped read-only, append()
    /// and advance_write_index() fail. Same checks and codes as open().
    int  open_read_only(const char* path, uint8_t tier, uint32_t record_size,
                        uint64_t capacity);
    void close();
    int  append(const void* record, size_t len);
    int  read_at(uint64_t index, void* out, size_t len) const;
    /// Counts `n` more records as written: records already in their slots
    /// that write_idx never reached (a crash between the pwrite and the
    /// index update). 0, or -1 if the ring is not open for writing.
    int  advance_write_index(uint64_t n);
    uint64_t write_index() const;
    uint64_t count() const;
    uint64_t capacity() const;
    uint8_t  tier() const;

    /// What a ring file's header says, read without opening it for
    /// writing (fsck learns tier, record size and capacity from it).
    struct Header {
        uint8_t  tier        = 0;
        uint32_t record_size = 0;
        uint64_t capacity    = 0;
        uint64_t write_idx   = 0;
    };
    /// 0, or an open() code: -3 open failed / -4 fstat failed (errno in
    /// *err), -13 shorter than a header, -8 bad magic, -9 version,
    /// -6 file size does not match record size x capacity.
    static int read_header(const char* path, Header* out, int* err);

    /// Why the last open() failed. describe() turns an open() return
    /// code into words; last_errno() is the errno of the failed syscall
    /// behind -3/-4/-5/-7 (0 for the format mismatches). Together they
    /// give an operator "open failed: Permission denied" rather than
    /// "rc=-3".
    static const char* describe(int rc);
    int                last_errno() const;

private:
    int  open_impl(const char* path, uint8_t tier, uint32_t record_size,
                   uint64_t capacity, bool read_only);

    int       fd_ = -1;
    bool      read_only_ = false;
    uint8_t   tier_ = 0;
    int       last_errno_ = 0;
    void*     mmap_base_ = nullptr;
    size_t    mmap_len_ = 0;
    uint32_t  record_size_ = 0;
    uint64_t  capacity_ = 0;
};

} // namespace budyk
