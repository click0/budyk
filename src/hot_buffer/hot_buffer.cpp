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
    // TODO: copy oldest-first from circular buffer
    (void)out; (void)out_cap;
    return 0;
}

void HotBuffer::reset() { head_ = 0; count_ = 0; }
size_t HotBuffer::size() const { return count_; }

} // namespace budyk
