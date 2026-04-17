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

private:
    int       fd_ = -1;
    void*     mmap_base_ = nullptr;
    size_t    mmap_len_ = 0;
    uint32_t  record_size_ = 0;
    uint64_t  capacity_ = 0;
};

} // namespace budyk
