// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

#include <cstddef>

namespace budyk {

// Folds a window of L3 samples into one L2- or L1-level sample
// (spec §3.4.1: "the aggregator periodically folds L3 samples into
// records for the L2 ring ... and L1 ring").
//
// Aggregation policy (MVP):
//   * percent / load fields : arithmetic mean
//   * mem.available, swap.used : arithmetic mean (rounded)
//   * mem.total, swap.total, cpu.count, uptime_seconds : last
//   * timestamp_nanos : last (end of window)
//   * level : `out_level` chosen at construction
//
// The aggregator is single-threaded and stateless across folds;
// `fold()` produces the summary and resets internal accumulators.
class TierAggregator {
public:
    explicit TierAggregator(Level out_level);

    void   add(const Sample& s);
    size_t size() const;

    // Produces one aggregated sample; returns false if no samples.
    // Always resets the accumulator, even on a no-op call.
    bool   fold(Sample* out);

    void   reset();

private:
    Level    out_level_;
    size_t   n_;

    double   cpu_total_percent_sum_;
    double   mem_available_sum_;
    double   mem_available_percent_sum_;
    double   swap_used_sum_;
    double   swap_used_percent_sum_;
    double   load_1m_sum_;
    double   load_5m_sum_;
    double   load_15m_sum_;

    Sample   last_;
};

} // namespace budyk
