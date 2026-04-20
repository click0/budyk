// SPDX-License-Identifier: BSD-3-Clause
#include "ai/baseline.h"
#include "ai/suggest.h"
#include "core/sample.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace budyk;

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // 1. Empty baseline yields empty per-metric output.
    {
        MetricBaseline empty{};
        assert(suggest_rule_cpu_total_percent    (empty)       == "");
        assert(suggest_rule_mem_available_percent(empty)       == "");
        assert(suggest_rule_swap_used_percent    (empty)       == "");
        assert(suggest_rule_load_1m              (empty, 4.0)  == "");
    }

    // 2. CPU threshold = max(60, ceil(p99+15)); p99=45 → 60.
    {
        MetricBaseline b{};
        b.n = 100; b.min = 5; b.max = 55; b.mean = 20; b.stddev = 10;
        b.p95 = 40; b.p99 = 45;
        std::string r = suggest_rule_cpu_total_percent(b);
        assert(!r.empty());
        assert(contains(r, "watch(\"high_cpu\""));
        assert(contains(r, "cpu.total_percent > 60"));
        assert(contains(r, "p99=45.0"));
    }

    // 3. CPU threshold capped at 95 when p99 is already high.
    {
        MetricBaseline b{};
        b.n = 100; b.max = 99; b.p99 = 90;
        std::string r = suggest_rule_cpu_total_percent(b);
        assert(contains(r, "cpu.total_percent > 95"));
    }

    // 4. Memory: min dipped, but floor(min-2)=4 clamps to 5 (min floor).
    {
        MetricBaseline b{};
        b.n = 100; b.min = 6.2; b.max = 70; b.mean = 40;
        std::string r = suggest_rule_mem_available_percent(b);
        assert(contains(r, "watch(\"memory_low\""));
        assert(contains(r, "severity = \"critical\""));
        assert(contains(r, "mem.available_percent < 5"));
    }

    // 4b. Memory: floor(min-2) above the 5% floor — uses the computed value.
    {
        MetricBaseline b{};
        b.n = 100; b.min = 9.8; b.max = 70; b.mean = 40;
        std::string r = suggest_rule_mem_available_percent(b);
        assert(contains(r, "mem.available_percent < 7"));
    }

    // 5. Memory stayed healthy → default 10, warning.
    {
        MetricBaseline b{};
        b.n = 100; b.min = 30; b.mean = 50;
        std::string r = suggest_rule_mem_available_percent(b);
        assert(contains(r, "mem.available_percent < 10"));
        assert(contains(r, "severity = \"warning\""));
    }

    // 6. Swap never used → no rule.
    {
        MetricBaseline b{};
        b.n = 100; b.max = 0.0;
        assert(suggest_rule_swap_used_percent(b) == "");
    }

    // 7. Swap pressure seen → rule at ceil(p99*2), floor 50.
    {
        MetricBaseline b{};
        b.n = 100; b.max = 40; b.p99 = 35;
        std::string r = suggest_rule_swap_used_percent(b);
        assert(contains(r, "watch(\"swap_pressure\""));
        assert(contains(r, "swap.used_percent > 70"));
    }

    // 8. Load: threshold = max(ceil(p99*1.5), cpu_count*1.5, 2.0).
    {
        MetricBaseline b{};
        b.n = 100; b.max = 6.0; b.p99 = 4.0;
        std::string r = suggest_rule_load_1m(b, /*cpu_count*/ 8);
        assert(contains(r, "watch(\"load_high\""));
        // cpu_count*1.5 = 12 dominates ceil(p99*1.5)=6 → threshold = 12.
        assert(contains(r, "load.avg_1m > 12"));
    }

    // 9. Full document from a Sample array covers all four metrics.
    {
        Sample s[10]{};
        for (int i = 0; i < 10; ++i) {
            s[i].cpu.total_percent     = 40.0 + i;
            s[i].cpu.count             = 4;
            s[i].mem.available_percent = 30.0 - i;
            s[i].swap.used_percent     = 10.0 + i;
            s[i].load.avg_1m           = 0.5 + 0.1 * i;
        }
        std::string doc = suggest_rules_for_samples(s, 10);
        assert(contains(doc, "AI-suggested rules (Tier A"));
        assert(contains(doc, "high_cpu"));
        assert(contains(doc, "memory_low"));
        assert(contains(doc, "swap_pressure"));
        assert(contains(doc, "load_high"));
    }

    // 10. Null Sample array → header-only document, no rules.
    {
        std::string doc = suggest_rules_for_samples(nullptr, 0);
        assert(contains(doc, "AI-suggested rules"));
        assert(!contains(doc, "watch("));
    }

    // 11. Disk / net — idle metric → no rule.
    {
        MetricBaseline b{};
        b.n = 100; b.max = 512.0 * 1024; b.p99 = 256.0 * 1024;  // KiB-scale
        assert(suggest_rule_disk_read_bytes_per_sec(b) == "");   // < 1 MiB/s
        assert(suggest_rule_net_rx_bytes_per_sec    (b) != "");  // ≥ 0.1 MiB/s
    }

    // 12. Disk read — active, threshold comes from p99 * 1.5, floored at 50 MiB/s.
    {
        MetricBaseline b{};
        b.n = 100;
        b.max = 200.0 * 1024 * 1024;   // 200 MiB/s
        b.p99 = 150.0 * 1024 * 1024;   // 150 MiB/s → t = ceil(225 MiB/s)
        std::string r = suggest_rule_disk_read_bytes_per_sec(b);
        assert(!r.empty());
        assert(contains(r, "watch(\"disk_read_high\""));
        assert(contains(r, "disk.read_bytes_per_sec >"));
        assert(contains(r, "MiB/s"));   // pretty-printed rationale
    }

    // 13. Disk write — low but active; p99*1.5 below floor → floor wins.
    {
        MetricBaseline b{};
        b.n = 100;
        b.max = 5.0  * 1024 * 1024;    // 5 MiB/s
        b.p99 = 2.0  * 1024 * 1024;    // p99*1.5 = 3 MiB/s  << 50 MiB/s floor
        std::string r = suggest_rule_disk_write_bytes_per_sec(b);
        assert(!r.empty());
        assert(contains(r, "watch(\"disk_write_high\""));
        assert(contains(r, "50.0 MiB/s"));
    }

    // 14. Net rx — active; threshold = max(ceil(p99 * 1.5), 5 MiB/s).
    {
        MetricBaseline b{};
        b.n = 100;
        b.max = 50.0 * 1024 * 1024;
        b.p99 = 20.0 * 1024 * 1024;    // p99*1.5 = 30 MiB/s > 5 MiB/s floor
        std::string r = suggest_rule_net_rx_bytes_per_sec(b);
        assert(!r.empty());
        assert(contains(r, "watch(\"net_rx_high\""));
        assert(contains(r, "net.rx_bytes_per_sec >"));
    }

    // 15. Full document from Sample array covers disk + net rules too.
    {
        Sample s[10]{};
        for (int i = 0; i < 10; ++i) {
            s[i].cpu.total_percent     = 40.0 + i;
            s[i].cpu.count             = 4;
            s[i].mem.available_percent = 30.0 - i;
            s[i].swap.used_percent     = 10.0 + i;
            s[i].load.avg_1m           = 0.5 + 0.1 * i;
            s[i].disk.read_bytes_per_sec  = (uint64_t)(100ULL*1024*1024 + i*1024*1024);
            s[i].disk.write_bytes_per_sec = (uint64_t)( 60ULL*1024*1024 + i*1024*1024);
            s[i].net.rx_bytes_per_sec     = (uint64_t)( 20ULL*1024*1024 + i*1024*1024);
            s[i].net.tx_bytes_per_sec     = (uint64_t)( 10ULL*1024*1024 + i*1024*1024);
        }
        std::string doc = suggest_rules_for_samples(s, 10);
        assert(contains(doc, "disk_read_high"));
        assert(contains(doc, "disk_write_high"));
        assert(contains(doc, "net_rx_high"));
        assert(contains(doc, "net_tx_high"));
    }

    std::printf("test_suggest: PASS\n");
    return 0;
}
