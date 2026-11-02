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

size_t HotBuffer::dump_last(Sample* out, size_t n) const {
    if (out == nullptr || n == 0 || count_ == 0) return 0;
    if (n > count_) n = count_;
    const size_t start = (head_ + capacity_ - n) % capacity_;
    for (size_t i = 0; i < n; ++i) {
        out[i] = ring_[(start + i) % capacity_];
    }
    return n;
}

void HotBuffer::reset() { head_ = 0; count_ = 0; }

bool WarmGrace::should_reset(int clients, uint64_t now_ns, bool fast) {
    connected_ = clients > 0;
    // A fast tick without a client (an escalation at 1 Hz for ten
    // minutes at 03:00) fills the buffer as a session does, and nothing
    // emptied it: the dashboard opened at 09:00 got its 290 samples.
    if (connected_ || fast) {
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

// Not while a client is connected: the spell starts at the first tick
// without one. The grace counted from the last tick with a client capped
// every sleep at it, so a warm_grace of 0 ran the tick in a loop (400
// samples a second) and one shorter than the interval added ticks.
uint64_t WarmGrace::ns_until_due(uint64_t now_ns) const {
    if (!pending_ || connected_) return UINT64_MAX;
    if (now_ns < last_client_ns_) return grace_ns_;
    const uint64_t idle = now_ns - last_client_ns_;
    return idle >= grace_ns_ ? 0 : grace_ns_ - idle;
}

int WarmGrace::cap_sleep_ms(int sleep_ms, uint64_t now_ns, int floor_ms) const {
    const uint64_t due_ns = ns_until_due(now_ns);
    if (due_ns == UINT64_MAX) return sleep_ms;
    uint64_t due_ms = due_ns / 1000000 + 1;
    if (floor_ms > 0 && due_ms < static_cast<uint64_t>(floor_ms)) due_ms = static_cast<uint64_t>(floor_ms);
    return sleep_ms > 0 && due_ms < static_cast<uint64_t>(sleep_ms) ? static_cast<int>(due_ms) : sleep_ms;
}
size_t HotBuffer::size() const { return count_; }

} // namespace budyk
