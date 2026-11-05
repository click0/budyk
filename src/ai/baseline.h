// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace budyk {

/// Summary statistics over a window of values (spec §3.7.1 Tier A).
/// p95 / p99 use the nearest-rank method on a sorted copy of the input;
/// with weights, the rank is in weight (the value at which the weight
/// so far reaches p percent of the total), the mean and the stddev are
/// weighted, min and max are not. `n` is the count either way.
struct MetricBaseline {
    double min;
    double max;
    double mean;
    double stddev;
    double p95;
    double p99;
    size_t n;
};

/// Compute stats over a plain array of doubles, each with a weight when
/// `weights` is given (`n` of them, each > 0). An empty input produces
/// a zeroed struct with n == 0.
MetricBaseline compute_stats(const double* values, size_t n, const double* weights = nullptr);

/// cpu.count of the newest sample that has the cpu group; 0 if none does.
uint32_t newest_cpu_count(const Sample* s, size_t n);

/// The weight of each sample for the statistics: the time it stands
/// for, the seconds from the one before it (the samples in time order),
/// at most `max_ns` (a gap while the daemon was down stands for
/// nothing); the first is `max_ns`. The L2 ring holds a record each 30
/// s and the L1 ring one each 5 minutes: counted alike, an L2 stretch
/// (a dashboard open, an anomaly: the least baseline-like hours) weighed
/// ten times what the same time at L1 did, and pushed every p99 up.
std::vector<double> sample_weights(const Sample* s, size_t n, uint64_t max_ns);

/// Extract a scalar metric from a Sample array and compute its stats,
/// over the samples that have its group (Sample::groups), each weighted
/// by `weights[i]` when given (`n` of them, as sample_weights() makes).
MetricBaseline compute_cpu_total_percent_stats       (const Sample* s, size_t n, const double* weights = nullptr);
MetricBaseline compute_mem_available_percent_stats   (const Sample* s, size_t n, const double* weights = nullptr);
MetricBaseline compute_swap_used_percent_stats       (const Sample* s, size_t n, const double* weights = nullptr);
MetricBaseline compute_load_1m_stats                 (const Sample* s, size_t n, const double* weights = nullptr);
MetricBaseline compute_disk_read_bytes_per_sec_stats (const Sample* s, size_t n, const double* weights = nullptr);
MetricBaseline compute_disk_write_bytes_per_sec_stats(const Sample* s, size_t n, const double* weights = nullptr);
MetricBaseline compute_net_rx_bytes_per_sec_stats    (const Sample* s, size_t n, const double* weights = nullptr);
MetricBaseline compute_net_tx_bytes_per_sec_stats    (const Sample* s, size_t n, const double* weights = nullptr);

} // namespace budyk
