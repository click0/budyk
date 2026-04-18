// SPDX-License-Identifier: BSD-3-Clause
#include "storage/tier_aggregator.h"

#include <cstring>

namespace budyk {

TierAggregator::TierAggregator(Level out_level)
    : out_level_(out_level) {
    reset();
}

void TierAggregator::reset() {
    n_                          = 0;
    cpu_total_percent_sum_      = 0.0;
    mem_available_sum_          = 0.0;
    mem_available_percent_sum_  = 0.0;
    swap_used_sum_              = 0.0;
    swap_used_percent_sum_      = 0.0;
    load_1m_sum_                = 0.0;
    load_5m_sum_                = 0.0;
    load_15m_sum_               = 0.0;
    std::memset(&last_, 0, sizeof(last_));
}

void TierAggregator::add(const Sample& s) {
    cpu_total_percent_sum_     += s.cpu.total_percent;
    mem_available_sum_         += static_cast<double>(s.mem.available);
    mem_available_percent_sum_ += s.mem.available_percent;
    swap_used_sum_             += static_cast<double>(s.swap.used);
    swap_used_percent_sum_     += s.swap.used_percent;
    load_1m_sum_               += s.load.avg_1m;
    load_5m_sum_               += s.load.avg_5m;
    load_15m_sum_              += s.load.avg_15m;
    last_ = s;
    ++n_;
}

size_t TierAggregator::size() const { return n_; }

bool TierAggregator::fold(Sample* out) {
    if (out == nullptr) { reset(); return false; }
    if (n_ == 0)        { reset(); return false; }

    const double inv_n = 1.0 / static_cast<double>(n_);

    Sample r{};
    r.timestamp_nanos = last_.timestamp_nanos;
    r.level           = out_level_;

    r.cpu.total_percent = cpu_total_percent_sum_ * inv_n;
    r.cpu.count         = last_.cpu.count;

    r.mem.total             = last_.mem.total;
    r.mem.available         = static_cast<uint64_t>(mem_available_sum_ * inv_n + 0.5);
    r.mem.available_percent = mem_available_percent_sum_ * inv_n;

    r.swap.total        = last_.swap.total;
    r.swap.used         = static_cast<uint64_t>(swap_used_sum_ * inv_n + 0.5);
    r.swap.used_percent = swap_used_percent_sum_ * inv_n;

    r.load.avg_1m  = load_1m_sum_  * inv_n;
    r.load.avg_5m  = load_5m_sum_  * inv_n;
    r.load.avg_15m = load_15m_sum_ * inv_n;

    r.uptime_seconds = last_.uptime_seconds;

    *out = r;
    reset();
    return true;
}

} // namespace budyk
