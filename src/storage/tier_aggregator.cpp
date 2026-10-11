// SPDX-License-Identifier: BSD-3-Clause
#include "storage/tier_aggregator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace budyk {

namespace {

uint64_t mean_u64(double sum, double inv_n) {
    return static_cast<uint64_t>(std::llround(sum * inv_n));
}

} // namespace

TierAggregator::TierAggregator(Level out_level)
    : out_level_(out_level) {
    reset();
}

void TierAggregator::reset() {
    n_                          = 0;
    first_ts_                   = 0;
    cpu_total_percent_sum_      = 0.0;
    mem_available_sum_          = 0.0;
    mem_available_percent_sum_  = 0.0;
    swap_used_sum_              = 0.0;
    swap_used_percent_sum_      = 0.0;
    load_1m_sum_                = 0.0;
    load_5m_sum_                = 0.0;
    load_15m_sum_               = 0.0;
    disk_read_sum_              = 0.0;
    disk_write_sum_             = 0.0;
    net_rx_sum_                 = 0.0;
    net_tx_sum_                 = 0.0;
    proc_total_max_             = 0;
    proc_running_max_           = 0;
    thermal_max_                = 0.0;
    rss_max_                    = 0;
    entropy_min_                = UINT32_MAX;
    file_events_sum_            = 0;
    entropy_present_            = false;
    thermal_present_            = false;
    file_watch_present_         = false;
    last_ = Sample{};
}

void TierAggregator::add(const Sample& s) {
    if (n_ == 0) first_ts_ = s.timestamp_nanos;

    cpu_total_percent_sum_     += s.cpu.total_percent;
    mem_available_sum_         += static_cast<double>(s.mem.available);
    mem_available_percent_sum_ += s.mem.available_percent;
    swap_used_sum_             += static_cast<double>(s.swap.used);
    swap_used_percent_sum_     += s.swap.used_percent;
    load_1m_sum_               += s.load.avg_1m;
    load_5m_sum_               += s.load.avg_5m;
    load_15m_sum_              += s.load.avg_15m;
    disk_read_sum_             += static_cast<double>(s.disk.read_bytes_per_sec);
    disk_write_sum_            += static_cast<double>(s.disk.write_bytes_per_sec);
    net_rx_sum_                += static_cast<double>(s.net.rx_bytes_per_sec);
    net_tx_sum_                += static_cast<double>(s.net.tx_bytes_per_sec);

    proc_total_max_   = std::max(proc_total_max_,   s.proc.total);
    proc_running_max_ = std::max(proc_running_max_, s.proc.running);
    rss_max_          = std::max(rss_max_,          s.self_.rss_bytes);
    if (s.thermal.present) {
        thermal_max_     = thermal_present_ ? std::max(thermal_max_, s.thermal.max_celsius)
                                            : s.thermal.max_celsius;
        thermal_present_ = true;
    }
    if (s.entropy.present) {
        entropy_min_     = std::min(entropy_min_, s.entropy.available_bits);
        entropy_present_ = true;
    }
    file_events_sum_    += s.file_watch.events_this_tick;
    file_watch_present_  = file_watch_present_ || s.file_watch.present;

    last_ = s;
    ++n_;
}

size_t TierAggregator::size() const { return n_; }

uint64_t TierAggregator::first_timestamp() const { return first_ts_; }

bool TierAggregator::fold(Sample* out) {
    if (out == nullptr) { reset(); return false; }
    if (n_ == 0)        { reset(); return false; }

    const double inv_n = 1.0 / static_cast<double>(n_);

    // Start from the last sample: every "last" field is already right.
    Sample r = last_;
    r.level  = out_level_;
    r.folded = true;

    r.cpu.total_percent     = cpu_total_percent_sum_ * inv_n;
    r.mem.available         = mean_u64(mem_available_sum_, inv_n);
    r.mem.available_percent = mem_available_percent_sum_ * inv_n;
    r.swap.used             = mean_u64(swap_used_sum_, inv_n);
    r.swap.used_percent     = swap_used_percent_sum_ * inv_n;
    r.load.avg_1m           = load_1m_sum_  * inv_n;
    r.load.avg_5m           = load_5m_sum_  * inv_n;
    r.load.avg_15m          = load_15m_sum_ * inv_n;
    r.disk.read_bytes_per_sec  = mean_u64(disk_read_sum_,  inv_n);
    r.disk.write_bytes_per_sec = mean_u64(disk_write_sum_, inv_n);
    r.net.rx_bytes_per_sec     = mean_u64(net_rx_sum_, inv_n);
    r.net.tx_bytes_per_sec     = mean_u64(net_tx_sum_, inv_n);

    r.proc.total          = proc_total_max_;
    r.proc.running        = proc_running_max_;
    r.self_.rss_bytes     = rss_max_;
    r.thermal.present     = thermal_present_;
    r.thermal.max_celsius = thermal_present_ ? thermal_max_ : 0.0;
    r.entropy.present        = entropy_present_;
    r.entropy.available_bits = entropy_present_ ? entropy_min_ : 0;
    r.file_watch.present          = file_watch_present_;
    r.file_watch.events_this_tick = static_cast<uint32_t>(
        std::min<uint64_t>(file_events_sum_, UINT32_MAX));

    *out = r;
    reset();
    return true;
}

} // namespace budyk
