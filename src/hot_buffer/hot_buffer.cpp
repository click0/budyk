// SPDX-License-Identifier: BSD-3-Clause
#include "hot_buffer/hot_buffer.h"
#include <cstring>

namespace budyk {

HotBuffer::HotBuffer(size_t capacity)
    : capacity_(capacity), head_(0), count_(0) {
    ring_ = new Sample[capacity_];
}

HotBuffer::~HotBuffer() { delete[] ring_; }

void HotBuffer::push(const Sample& s) {
    ring_[head_] = s;
    head_ = (head_ + 1) % capacity_;
    if (count_ < capacity_) ++count_;
}

size_t HotBuffer::dump(Sample* out, size_t out_cap) const {
    if (out == nullptr || out_cap == 0 || count_ == 0) return 0;
    const size_t n     = count_ < out_cap ? count_ : out_cap;
    const size_t start = (head_ + capacity_ - count_) % capacity_;
    for (size_t i = 0; i < n; ++i) {
        out[i] = ring_[(start + i) % capacity_];
    }
    return n;
}

void HotBuffer::reset() { head_ = 0; count_ = 0; }
size_t HotBuffer::size() const { return count_; }

} // namespace budyk
