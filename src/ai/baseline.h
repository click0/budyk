// SPDX-License-Identifier: BSD-3-Clause
#pragma once
namespace budyk {
struct MetricBaseline {
    double mean, stddev, p95, p99, min, max;
};
int compute_baseline(const char* data_dir, int window_days, MetricBaseline* out, int max_metrics);
} // namespace budyk
