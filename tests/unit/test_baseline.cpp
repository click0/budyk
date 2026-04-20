// SPDX-License-Identifier: BSD-3-Clause
#include "ai/baseline.h"
#include "core/sample.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>

using namespace budyk;

static bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

int main() {
    // 1. Empty input yields all-zero stats with n == 0.
    {
        MetricBaseline b = compute_stats(nullptr, 0);
        assert(b.n == 0);
        assert(b.min == 0.0 && b.max == 0.0 && b.mean == 0.0);
        assert(b.stddev == 0.0 && b.p95 == 0.0 && b.p99 == 0.0);
    }

    // 2. Single value — all point stats equal that value.
    {
        double x = 42.0;
        MetricBaseline b = compute_stats(&x, 1);
        assert(b.n == 1);
        assert(near(b.min, 42.0) && near(b.max, 42.0) && near(b.mean, 42.0));
        assert(near(b.stddev, 0.0));
        assert(near(b.p95, 42.0) && near(b.p99, 42.0));
    }

    // 3. Constant vector — stddev = 0, percentiles equal the value.
    {
        double v[10];
        for (auto& x : v) x = 7.0;
        MetricBaseline b = compute_stats(v, 10);
        assert(b.n == 10);
        assert(near(b.mean, 7.0));
        assert(near(b.stddev, 0.0));
        assert(near(b.p95, 7.0));
    }

    // 4. Known sequence 1..10 — mean=5.5, stddev=~2.872, min=1, max=10.
    //    Percentiles via nearest-rank: p95 → index ceil(0.95*10)-1 = 9 → 10;
    //    p99 → index ceil(0.99*10)-1 = 9 → 10.
    {
        double v[10];
        for (int i = 0; i < 10; ++i) v[i] = static_cast<double>(i + 1);
        MetricBaseline b = compute_stats(v, 10);
        assert(b.n == 10);
        assert(near(b.min, 1.0));
        assert(near(b.max, 10.0));
        assert(near(b.mean, 5.5));
        assert(near(b.stddev, std::sqrt(8.25), 1e-9));
        assert(near(b.p95, 10.0));
        assert(near(b.p99, 10.0));
    }

    // 5. Larger sequence — p95 and p99 land inside the range.
    //    v = 1..100. p95: idx = 94 → value 95; p99: idx = 98 → value 99.
    {
        double v[100];
        for (int i = 0; i < 100; ++i) v[i] = static_cast<double>(i + 1);
        MetricBaseline b = compute_stats(v, 100);
        assert(b.n == 100);
        assert(near(b.p95, 95.0));
        assert(near(b.p99, 99.0));
    }

    // 6. Sample extractors — cpu.total_percent pulled and aggregated.
    {
        Sample s[5]{};
        for (int i = 0; i < 5; ++i) {
            s[i].cpu.total_percent     = 10.0 * (i + 1);
            s[i].mem.available_percent = 20.0 + i;
            s[i].swap.used_percent     = 0.0;
            s[i].load.avg_1m           = 0.5 * (i + 1);
        }
        MetricBaseline cpu  = compute_cpu_total_percent_stats   (s, 5);
        MetricBaseline mem  = compute_mem_available_percent_stats(s, 5);
        MetricBaseline swap = compute_swap_used_percent_stats    (s, 5);
        MetricBaseline load = compute_load_1m_stats              (s, 5);

        assert(cpu.n == 5 && near(cpu.mean, 30.0));   // mean of 10,20,30,40,50
        assert(near(mem.mean, 22.0));                 // mean of 20..24
        assert(near(swap.mean, 0.0) && near(swap.stddev, 0.0));
        assert(near(load.mean, 1.5));                 // mean of 0.5..2.5
        assert(near(cpu.max, 50.0) && near(cpu.min, 10.0));
    }

    // 7. Null Sample pointer / zero count — safe empty result.
    {
        MetricBaseline b = compute_cpu_total_percent_stats(nullptr, 10);
        assert(b.n == 0);
        Sample s{};
        b = compute_cpu_total_percent_stats(&s, 0);
        assert(b.n == 0);
    }

    // 8. Disk + net extractors — uint64 throughput pulled into double stats.
    {
        Sample s[5]{};
        for (int i = 0; i < 5; ++i) {
            s[i].disk.read_bytes_per_sec  = static_cast<uint64_t>(1'000'000ULL * (i + 1));
            s[i].disk.write_bytes_per_sec = static_cast<uint64_t>(  500'000ULL * (i + 1));
            s[i].net.rx_bytes_per_sec     = static_cast<uint64_t>(2'000'000ULL * (i + 1));
            s[i].net.tx_bytes_per_sec     = static_cast<uint64_t>(  750'000ULL * (i + 1));
        }
        MetricBaseline dr = compute_disk_read_bytes_per_sec_stats (s, 5);
        MetricBaseline dw = compute_disk_write_bytes_per_sec_stats(s, 5);
        MetricBaseline nr = compute_net_rx_bytes_per_sec_stats    (s, 5);
        MetricBaseline nt = compute_net_tx_bytes_per_sec_stats    (s, 5);

        assert(dr.n == 5 && near(dr.mean, 3'000'000.0));  // 1M..5M mean = 3M
        assert(near(dw.mean, 1'500'000.0));
        assert(near(nr.mean, 6'000'000.0));
        assert(near(nt.mean, 2'250'000.0));
        assert(near(dr.max, 5'000'000.0));
        assert(near(nr.min, 2'000'000.0));
    }

    std::printf("test_baseline: PASS\n");
    return 0;
}
