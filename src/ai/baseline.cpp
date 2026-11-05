// SPDX-License-Identifier: BSD-3-Clause
#include "ai/baseline.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace budyk {

namespace {

// A value and its weight, sorted by value.
using Weighted = std::pair<double, double>;

// Nearest-rank percentile on presorted (value, weight) pairs: the value
// at which the weight so far first reaches p percent of the total. With
// every weight 1 it is the standard definition (rank ceil(p/100 * n)).
double nearest_rank(const std::vector<Weighted>& sorted, double total, double p) {
    const size_t n = sorted.size();
    if (n == 0) return 0.0;
    const double want = p * 0.01 * total;
    double so_far = 0.0;
    for (const auto& vw : sorted) {
        so_far += vw.second;
        // A hair under, from the sum's rounding, is at the rank.
        if (so_far >= want - total * 1e-12) return vw.first;
    }
    return sorted[n - 1].first;
}

MetricBaseline compute_from_vector(std::vector<Weighted>& vals) {
    MetricBaseline b{};
    b.n = vals.size();
    if (b.n == 0) return b;

    double minv = vals[0].first, maxv = vals[0].first, sum = 0.0, total = 0.0;
    for (const auto& vw : vals) {
        if (vw.first < minv) minv = vw.first;
        if (vw.first > maxv) maxv = vw.first;
        sum   += vw.first * vw.second;
        total += vw.second;
    }
    if (total <= 0.0) total = 1.0;
    b.min  = minv;
    b.max  = maxv;
    b.mean = sum / total;

    double ss = 0.0;
    for (const auto& vw : vals) {
        const double d = vw.first - b.mean;
        ss += d * d * vw.second;
    }
    b.stddev = std::sqrt(ss / total);

    std::sort(vals.begin(), vals.end(),
              [](const Weighted& a, const Weighted& c) { return a.first < c.first; });
    b.p95 = nearest_rank(vals, total, 95.0);
    b.p99 = nearest_rank(vals, total, 99.0);
    return b;
}

// Over the samples that have `group`: in the others it is zeros, not
// readings (a level that does not collect it, a rate's first read).
template <typename Fn>
MetricBaseline extract_and_compute(const Sample* s, size_t n, const double* weights,
                                   uint32_t group, Fn extract) {
    if (s == nullptr || n == 0) return MetricBaseline{};
    std::vector<Weighted> vals;
    vals.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if ((s[i].groups & group) != 0) {
            vals.emplace_back(extract(s[i]), weights != nullptr ? weights[i] : 1.0);
        }
    }
    return compute_from_vector(vals);
}

} // namespace

MetricBaseline compute_stats(const double* values, size_t n, const double* weights) {
    std::vector<Weighted> v;
    if (values != nullptr && n > 0) {
        v.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            v.emplace_back(values[i], weights != nullptr ? weights[i] : 1.0);
        }
    }
    return compute_from_vector(v);
}

uint32_t newest_cpu_count(const Sample* s, size_t n) {
    for (size_t i = n; s != nullptr && i > 0; --i) {
        if ((s[i - 1].groups & kGroupCpu) != 0) return s[i - 1].cpu.count;
    }
    return 0;
}

std::vector<double> sample_weights(const Sample* s, size_t n, uint64_t max_ns) {
    std::vector<double> w;
    if (s == nullptr || n == 0) return w;
    w.reserve(n);
    const double cap_s = static_cast<double>(max_ns) / 1e9;
    w.push_back(cap_s);
    for (size_t i = 1; i < n; ++i) {
        const uint64_t prev = s[i - 1].timestamp_nanos, cur = s[i].timestamp_nanos;
        uint64_t gap = cur > prev ? cur - prev : 0;
        if (gap > max_ns) gap = max_ns;
        // Two records at the same instant: a weight of nothing is a
        // division by zero waiting; a millisecond.
        double ws = static_cast<double>(gap) / 1e9;
        if (ws < 0.001) ws = 0.001;
        w.push_back(ws);
    }
    return w;
}

MetricBaseline compute_cpu_total_percent_stats(const Sample* s, size_t n, const double* weights) {
    return extract_and_compute(s, n, weights, kGroupCpu,
                               [](const Sample& x) { return x.cpu.total_percent; });
}

MetricBaseline compute_mem_available_percent_stats(const Sample* s, size_t n, const double* weights) {
    return extract_and_compute(s, n, weights, kGroupMem,
                               [](const Sample& x) { return x.mem.available_percent; });
}

MetricBaseline compute_swap_used_percent_stats(const Sample* s, size_t n, const double* weights) {
    return extract_and_compute(s, n, weights, kGroupSwap,
                               [](const Sample& x) { return x.swap.used_percent; });
}

MetricBaseline compute_load_1m_stats(const Sample* s, size_t n, const double* weights) {
    return extract_and_compute(s, n, weights, kGroupLoad,
                               [](const Sample& x) { return x.load.avg_1m; });
}

MetricBaseline compute_disk_read_bytes_per_sec_stats(const Sample* s, size_t n, const double* weights) {
    return extract_and_compute(s, n, weights, kGroupDisk, [](const Sample& x) {
        return static_cast<double>(x.disk.read_bytes_per_sec);
    });
}

MetricBaseline compute_disk_write_bytes_per_sec_stats(const Sample* s, size_t n, const double* weights) {
    return extract_and_compute(s, n, weights, kGroupDisk, [](const Sample& x) {
        return static_cast<double>(x.disk.write_bytes_per_sec);
    });
}

MetricBaseline compute_net_rx_bytes_per_sec_stats(const Sample* s, size_t n, const double* weights) {
    return extract_and_compute(s, n, weights, kGroupNet, [](const Sample& x) {
        return static_cast<double>(x.net.rx_bytes_per_sec);
    });
}

MetricBaseline compute_net_tx_bytes_per_sec_stats(const Sample* s, size_t n, const double* weights) {
    return extract_and_compute(s, n, weights, kGroupNet, [](const Sample& x) {
        return static_cast<double>(x.net.tx_bytes_per_sec);
    });
}

} // namespace budyk
