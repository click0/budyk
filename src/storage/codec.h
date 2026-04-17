// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// On-disk record codec — wraps core/codec with CRC32C + level marker.
// See spec §3.5 for format.

#include "core/sample.h"

#include <cstddef>
#include <cstdint>

namespace budyk {

// Per-record on-disk layout:
//   u64  timestamp_nanos (LE)
//   u8   level
//   u8   _pad
//   u32  crc32c (LE)        covers everything except the CRC bytes themselves
//   u8[] payload            produced by sample_encode()
constexpr size_t kRecordHeaderSize = 14;

size_t record_size_for_sample();

int record_encode(const Sample& s, void* buf, size_t cap, size_t* out_len);
int record_decode(const void* buf, size_t len, Sample* out);

uint32_t crc32c(const void* data, size_t len, uint32_t seed = 0);

} // namespace budyk
