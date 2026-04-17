// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include <cstddef>

namespace budyk {
int    sample_encode(const Sample* s, void* buf, size_t cap, size_t* out_len);
int    sample_decode(const void* buf, size_t len, Sample* out);
size_t sample_max_encoded_size();
} // namespace budyk
