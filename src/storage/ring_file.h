// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <cstddef>

namespace budyk {

// On-disk ring-buffer file with fixed-size records.
// Header: 64 bytes (magic, version, tier, record_size, capacity, write_idx).
// Records: [0..capacity-1], each record_size bytes.
class RingFile {
public:
    int  open(const char* path, uint8_t tier, uint32_t record_size, uint64_t capacity);
    void close();
    int  append(const void* record, size_t len);
    int  read_at(uint64_t index, void* out, size_t len) const;
    uint64_t write_index() const;
    uint64_t count() const;
    uint64_t capacity() const;

    // Why the last open() failed. describe() turns an open() return
    // code into words; last_errno() is the errno of the failed syscall
    // behind -3/-4/-5/-7 (0 for the format mismatches). Together they
    // give an operator "open failed: Permission denied" rather than
    // "rc=-3".
    static const char* describe(int rc);
    int                last_errno() const;

private:
    int       fd_ = -1;
    int       last_errno_ = 0;
    void*     mmap_base_ = nullptr;
    size_t    mmap_len_ = 0;
    uint32_t  record_size_ = 0;
    uint64_t  capacity_ = 0;
};

} // namespace budyk
