// SPDX-License-Identifier: BSD-3-Clause
#include "hot_buffer/hot_buffer.h"
#include <cstring>

namespace budyk {

HotBuffer::HotBuffer(size_t capacity)
    : capacity_(capacity > 0 ? capacity : 1), head_(0), count_(0) {
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

bool WarmGrace::should_reset(int clients, uint64_t now_ns) {
    if (clients > 0) {
        last_client_ns_ = now_ns;
        pending_        = true;
        return false;
    }
    if (!pending_) return false;
    if (now_ns < last_client_ns_) {          // clock stepped back
        last_client_ns_ = now_ns;
        return false;
    }
    if (now_ns - last_client_ns_ < grace_ns_) return false;
    pending_ = false;
    return true;
}

uint64_t WarmGrace::ns_until_due(uint64_t now_ns) const {
    if (!pending_) return UINT64_MAX;
    if (now_ns < last_client_ns_) return grace_ns_;
    const uint64_t idle = now_ns - last_client_ns_;
    return idle >= grace_ns_ ? 0 : grace_ns_ - idle;
}
size_t HotBuffer::size() const { return count_; }

} // namespace budyk
