// SPDX-License-Identifier: BSD-3-Clause
#include "ai/baseline.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace budyk {

namespace {

// Nearest-rank percentile on a presorted vector (standard definition).
double nearest_rank(const std::vector<double>& sorted, double p) {
    const size_t n = sorted.size();
    if (n == 0) return 0.0;
    double rank = std::ceil(p * 0.01 * static_cast<double>(n));
    size_t idx  = rank < 1.0 ? 0 : static_cast<size_t>(rank) - 1;
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

MetricBaseline compute_from_vector(std::vector<double>& vals) {
    MetricBaseline b{};
    b.n = vals.size();
    if (b.n == 0) return b;

    double minv = vals[0], maxv = vals[0], sum = 0.0;
    for (double v : vals) {
        if (v < minv) minv = v;
        if (v > maxv) maxv = v;
        sum += v;
    }
    b.min  = minv;
    b.max  = maxv;
    b.mean = sum / static_cast<double>(b.n);

    double ss = 0.0;
    for (double v : vals) {
        const double d = v - b.mean;
        ss += d * d;
    }
    b.stddev = std::sqrt(ss / static_cast<double>(b.n));

    std::sort(vals.begin(), vals.end());
    b.p95 = nearest_rank(vals, 95.0);
    b.p99 = nearest_rank(vals, 99.0);
    return b;
}

template <typename Fn>
MetricBaseline extract_and_compute(const Sample* s, size_t n, Fn extract) {
    if (s == nullptr || n == 0) return MetricBaseline{};
    std::vector<double> vals;
    vals.reserve(n);
    for (size_t i = 0; i < n; ++i) vals.push_back(extract(s[i]));
    return compute_from_vector(vals);
}

} // namespace

MetricBaseline compute_stats(const double* values, size_t n) {
    std::vector<double> v;
    if (values != nullptr && n > 0) {
        v.reserve(n);
        v.insert(v.end(), values, values + n);
    }
    return compute_from_vector(v);
}

MetricBaseline compute_cpu_total_percent_stats(const Sample* s, size_t n) {
    return extract_and_compute(s, n, [](const Sample& x) { return x.cpu.total_percent; });
}

MetricBaseline compute_mem_available_percent_stats(const Sample* s, size_t n) {
    return extract_and_compute(s, n, [](const Sample& x) { return x.mem.available_percent; });
}

MetricBaseline compute_swap_used_percent_stats(const Sample* s, size_t n) {
    return extract_and_compute(s, n, [](const Sample& x) { return x.swap.used_percent; });
}

MetricBaseline compute_load_1m_stats(const Sample* s, size_t n) {
    return extract_and_compute(s, n, [](const Sample& x) { return x.load.avg_1m; });
}

MetricBaseline compute_disk_read_bytes_per_sec_stats(const Sample* s, size_t n) {
    return extract_and_compute(s, n, [](const Sample& x) {
        return static_cast<double>(x.disk.read_bytes_per_sec);
    });
}

MetricBaseline compute_disk_write_bytes_per_sec_stats(const Sample* s, size_t n) {
    return extract_and_compute(s, n, [](const Sample& x) {
        return static_cast<double>(x.disk.write_bytes_per_sec);
    });
}

MetricBaseline compute_net_rx_bytes_per_sec_stats(const Sample* s, size_t n) {
    return extract_and_compute(s, n, [](const Sample& x) {
        return static_cast<double>(x.net.rx_bytes_per_sec);
    });
}

MetricBaseline compute_net_tx_bytes_per_sec_stats(const Sample* s, size_t n) {
    return extract_and_compute(s, n, [](const Sample& x) {
        return static_cast<double>(x.net.tx_bytes_per_sec);
    });
}

} // namespace budyk
