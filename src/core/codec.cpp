// SPDX-License-Identifier: BSD-3-Clause
#include "core/codec.h"
namespace budyk {
int    sample_encode(const Sample*, void*, size_t, size_t*) { return -1; /* TODO */ }
int    sample_decode(const void*, size_t, Sample*)          { return -1; /* TODO */ }
size_t sample_max_encoded_size()                            { return 4096; }
} // namespace budyk
