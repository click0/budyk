// SPDX-License-Identifier: BSD-3-Clause
#include "ai/baseline.h"
#include "ai/suggest.h"
#include "core/sample.h"
#include "rules/lua_engine.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

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

    // 2. CPU threshold = max(85, ceil(p99+15)); p99=45 → 85, as spec
    //    §3.7.1 has it (the floor was 60). Five ticks in a row, as the
    //    spec's examples have it: one tick fired it, every other tick.
    {
        MetricBaseline b{};
        b.n = 100; b.min = 5; b.max = 55; b.mean = 20; b.stddev = 10;
        b.p95 = 40; b.p99 = 45;
        std::string r = suggest_rule_cpu_total_percent(b);
        assert(!r.empty());
        assert(contains(r, "watch(\"high_cpu\""));
        assert(contains(r, "cpu.total_percent > 85"));
        assert(contains(r, "for_ticks = 5,"));
        assert(contains(r, "p99=45.0"));
        b.p99 = 78;
        assert(contains(suggest_rule_cpu_total_percent(b), "cpu.total_percent > 93"));
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
        assert(contains(r, "severity  = \"critical\""));
        assert(contains(r, "mem.available_percent < 5"));
        assert(contains(r, "for_ticks = 3,"));
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
        assert(contains(r, "severity  = \"warning\""));
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
            s[i].groups = kGroupsAll;
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
        assert(contains(r, "threshold 50.0 MiB/s"));
        // The rate in words is in the rationale's comment, not after the
        // condition, where it commented out the function's `end`.
        assert(contains(r, "return disk.write_bytes_per_sec > 52428800 end,"));
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
            s[i].groups = kGroupsAll;
            s[i].cpu.total_percent     = 40.0 + i;
            s[i].cpu.count             = 4;
            s[i].mem.available_percent = 30.0 - i;
            s[i].swap.used_percent     = 10.0 + i;
            s[i].load.avg_1m           = 0.5 + 0.1 * i;
            s[i].disk.read_bytes_per_sec  = (uint64_t)(100ULL*1024*1024 + static_cast<unsigned long long>(i)*1024*1024);
            s[i].disk.write_bytes_per_sec = (uint64_t)( 60ULL*1024*1024 + static_cast<unsigned long long>(i)*1024*1024);
            s[i].net.rx_bytes_per_sec     = (uint64_t)( 20ULL*1024*1024 + static_cast<unsigned long long>(i)*1024*1024);
            s[i].net.tx_bytes_per_sec     = (uint64_t)( 10ULL*1024*1024 + static_cast<unsigned long long>(i)*1024*1024);
        }
        std::string doc = suggest_rules_for_samples(s, 10);
        assert(contains(doc, "disk_read_high"));
        assert(contains(doc, "disk_write_high"));
        assert(contains(doc, "net_rx_high"));
        assert(contains(doc, "net_tx_high"));
    }

    // 16. merge_l2_l1: each stretch once. L1 interval 300 s; an L2 stretch
    //     at 30-90 s; the raw L1 tick at 330 s after it (it was dropped:
    //     an L2 record within 300 s before it), a folded L1 record at 90 s
    //     (the stretch again: dropped), one at 1000 s whose L2 records are
    //     gone (kept), one at 2000 s with an L2 record at exactly 1700 s,
    //     outside its window (1700, 2000] (kept).
    {
        const uint64_t s = 1000000000ULL;
        auto rec = [&](uint64_t t, bool folded) {
            Sample x{};
            x.timestamp_nanos = t * s;
            x.folded = folded;
            return x;
        };
        std::vector<Sample> l2 = {rec(90, false), rec(30, false), rec(60, false),
                                  rec(1700, false)};
        std::vector<Sample> l1 = {rec(330, false), rec(0, false), rec(90, true),
                                  rec(2000, true), rec(1000, true)};
        const std::vector<Sample> m = merge_l2_l1(l2, l1, 300 * s);
        const uint64_t want[] = {0, 30, 60, 90, 330, 1000, 1700, 2000};
        assert(m.size() == sizeof(want) / sizeof(want[0]));
        for (size_t i = 0; i < m.size(); ++i) assert(m[i].timestamp_nanos == want[i] * s);
        assert(!m[3].folded);   // the L2 record at 90 s, not the folded L1 one
    }

    // 17. The document loads as the daemon loads a rules file, and its
    //     rules fire: all eight, disk and network among them (each of
    //     those had a `--` comment that took its function's `end`, and
    //     the file did not load: spec §5 item 14). Over the thresholds,
    //     memory_low fires on the 3rd tick and the rest on the 5th; under
    //     them, nothing does.
    {
        Sample h[100]{};
        for (int i = 0; i < 100; ++i) {
            h[i].groups = kGroupsAll;
            h[i].cpu.total_percent     = 30.0 + i % 20;
            h[i].cpu.count             = 4;
            h[i].mem.available_percent = 40.0 - i % 35;   // dips to 6 %
            h[i].swap.used_percent     = 5.0 + i % 10;
            h[i].load.avg_1m           = 1.0 + 0.01 * i;
            h[i].disk.read_bytes_per_sec  = (80ULL + static_cast<unsigned>(i)) << 20;
            h[i].disk.write_bytes_per_sec = (40ULL + static_cast<unsigned>(i)) << 20;
            h[i].net.rx_bytes_per_sec     = (10ULL + static_cast<unsigned>(i)) << 20;
            h[i].net.tx_bytes_per_sec     = (5ULL  + static_cast<unsigned>(i)) << 20;
        }
        const std::string doc = suggest_rules_for_samples(h, 100);
        std::string why;
        assert(check_suggested_rules(doc, &why) == 8);

        LuaEngine e;
        assert(e.init(false) == 0);
        if (e.load_string(doc.c_str()) != 0) {
            std::fprintf(stderr, "%s\n%s\n", doc.c_str(), e.last_error().c_str());
            assert(false);
        }
        assert(e.rule_count() == 8);
        Sample quiet = h[0];
        quiet.mem.available_percent = 50;
        for (int t = 0; t < 6; ++t) assert(e.eval_tick(quiet) == 0);

        Sample hot{};
        hot.groups = kGroupsAll;
        hot.cpu.total_percent = 100; hot.cpu.count = 4;
        hot.mem.available_percent = 1;
        hot.swap.used_percent = 100;
        hot.load.avg_1m = 1000;
        hot.disk.read_bytes_per_sec = hot.disk.write_bytes_per_sec = 1ULL << 40;
        hot.net.rx_bytes_per_sec    = hot.net.tx_bytes_per_sec    = 1ULL << 40;
        const int fired[] = {0, 0, 1, 0, 7};   // memory_low at 3, the rest at 5
        for (int t = 0; t < 5; ++t) assert(e.eval_tick(hot) == fired[t]);
        for (const auto& r : e.rules()) assert(r.fire_count == 1);
    }

    // 18. A text that does not load is -1, with Lua's reason; a comment
    //     after a condition is what the throughput rules had.
    {
        std::string why;
        assert(check_suggested_rules(
            "watch(\"x\", { when = function() return cpu.total_percent > 1  -- 1 % end,\n"
            "  action = alert })\n", &why) == -1);
        assert(contains(why, "'end' expected"));
        assert(check_suggested_rules("-- nothing\n", &why) == 0);
    }

    std::printf("test_suggest: PASS\n");
    return 0;
}
