// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

#include <cstddef>

namespace budyk {

// Summary statistics over a window of values (spec §3.7.1 Tier A).
// p95 / p99 use the nearest-rank method on a sorted copy of the input.
struct MetricBaseline {
    double min;
    double max;
    double mean;
    double stddev;
    double p95;
    double p99;
    size_t n;
};

// Compute stats over a plain array of doubles. An empty input produces
// a zeroed struct with n == 0.
MetricBaseline compute_stats(const double* values, size_t n);

// Extract a scalar metric from a Sample array and compute its stats.
MetricBaseline compute_cpu_total_percent_stats    (const Sample* s, size_t n);
MetricBaseline compute_mem_available_percent_stats(const Sample* s, size_t n);
MetricBaseline compute_swap_used_percent_stats    (const Sample* s, size_t n);
MetricBaseline compute_load_1m_stats              (const Sample* s, size_t n);

} // namespace budyk
