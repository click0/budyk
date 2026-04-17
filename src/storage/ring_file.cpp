// SPDX-License-Identifier: BSD-3-Clause
#include "storage/ring_file.h"
namespace budyk {
int  RingFile::open(const char*, uint8_t, uint32_t, uint64_t) { return -1; /* TODO */ }
void RingFile::close() { /* TODO */ }
int  RingFile::append(const void*, size_t) { return -1; }
int  RingFile::read_at(uint64_t, void*, size_t) const { return -1; }
uint64_t RingFile::write_index() const { return 0; }
uint64_t RingFile::count() const { return 0; }
} // namespace budyk
