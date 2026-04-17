// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include <cstddef>

namespace budyk {

// Lock-free SPSC circular buffer for WS catch-up.
// Never writes to disk. Ephemeral.
class HotBuffer {
public:
    explicit HotBuffer(size_t capacity = 300);
    ~HotBuffer();

    void push(const Sample& s);

    // Dump all buffered samples into `out` (oldest first).
    // Returns number of samples written.
    size_t dump(Sample* out, size_t out_cap) const;

    void reset();
    size_t size() const;

private:
    Sample* ring_;
    size_t  capacity_;
    size_t  head_;
    size_t  count_;
};

} // namespace budyk
