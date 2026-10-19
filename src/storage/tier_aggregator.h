// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

#include <cstddef>
#include <cstdint>

namespace budyk {

// Folds a window of finer samples (L3, a custom level, or L2) into one
// L2- or L1-level record (spec §3.4.1: "the aggregator periodically
// folds L3 samples into records for the L2 ring ... and L1 ring").
// TierManager drives it; see TierManager::set_fold_windows().
//
// Per field (spec §1 leaves the choice open):
//   mean      cpu.*_percent, mem.available(+_percent),
//             swap.used(+_percent), load.*, disk.*_bytes_per_sec,
//             net.*_bytes_per_sec, tcp / udp / icmp.*_per_sec — what
//             the window looked like on average; byte counts are rounded
//   max       proc.* (every state, threads), tcp.established,
//             tcp.time_wait, thermal.max_celsius, self_.rss_bytes,
//             fs.max_used_percent, fs.max_inodes_used_percent,
//             clock.max_error_ms — a spike inside the window stays
//             visible; clock.offset_ms keeps the largest |offset|
//   min       entropy.available_bits — so does a drained pool
//   sum       file_watch.events_this_tick — events in the whole window
//   any       the present flags
//   all       clock.synced (of the samples that read the clock)
//   last      timestamp_nanos (the window's end), the counts and totals
//             (cpu.count, mem.total, swap.total, disk.device_count,
//             net.interface_count, thermal.sensor_count, fs.count,
//             file_watch.watched_count), uptime_seconds and the
//             cumulative self_ fields (peak_rss_bytes, cpu_*_seconds)
// Each group comes from the samples that have it (Sample::groups): its
// means divide by their number, its last fields are the last of them,
// and the result has every group some sample had. A group no sample had
// stays zero.
// The result has level `out_level` and folded = true.
//
// Single-threaded; fold() returns the summary and starts a new window.
class TierAggregator {
public:
    explicit TierAggregator(Level out_level);

    void   add(const Sample& s);
    size_t size() const;
    // Timestamp of the first sample in the current window (0 if empty).
    uint64_t first_timestamp() const;

    // Produces one aggregated sample; returns false if no samples.
    // Always resets the accumulator, even on a no-op call.
    bool   fold(Sample* out);

    void   reset();

private:
    static constexpr unsigned kGroupCount = 17;   // MetricGroup bits

    Level    out_level_;
    size_t   n_;
    uint64_t first_ts_;
    uint32_t group_n_[kGroupCount];   // samples that had each group

    // Means.
    double   cpu_total_percent_sum_;
    double   mem_available_sum_;
    double   mem_available_percent_sum_;
    double   swap_used_sum_;
    double   swap_used_percent_sum_;
    double   load_1m_sum_;
    double   load_5m_sum_;
    double   load_15m_sum_;
    double   disk_read_sum_;
    double   disk_write_sum_;
    double   net_rx_sum_;
    double   net_tx_sum_;
    double   cpu_part_sum_[6];      // user, system, iowait, irq, steal, idle
    double   tcp_rate_sum_[6];      // active, passive, retransmits, in, out, errors
    double   udp_rate_sum_[4];      // in, out, no port, errors
    double   icmp_rate_sum_[4];     // in, out, errors in, unreachable in

    // Extremes, sums, flags.
    uint32_t proc_total_max_;
    uint32_t proc_running_max_;
    double   thermal_max_;
    uint64_t rss_max_;
    uint32_t entropy_min_;
    uint64_t file_events_sum_;
    bool     entropy_present_;
    bool     thermal_present_;
    bool     file_watch_present_;
    ProcessStats proc_max_;
    TcpStats     tcp_;        // established / time_wait max, present
    bool         udp_present_;
    bool         icmp_present_;
    FsStats      fs_max_;     // the two percentages max, present
    ClockStats   clock_;      // offset (largest |x|), max error, synced, present

    Sample   last_;
};

} // namespace budyk
