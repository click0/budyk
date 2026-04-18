// SPDX-License-Identifier: BSD-3-Clause
#include "core/sample.h"
#include "storage/tier_aggregator.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace budyk;

static Sample mk(uint64_t ts, Level lv, double cpu_pct, double load1,
                 uint64_t mem_avail, double mem_avail_pct) {
    Sample s{};
    s.timestamp_nanos        = ts;
    s.level                  = lv;
    s.cpu.total_percent      = cpu_pct;
    s.cpu.count              = 4;
    s.mem.total              = 16ULL << 30;
    s.mem.available          = mem_avail;
    s.mem.available_percent  = mem_avail_pct;
    s.swap.total             = 2ULL << 30;
    s.swap.used              = 0;
    s.swap.used_percent      = 0.0;
    s.load.avg_1m            = load1;
    s.load.avg_5m            = load1 * 0.8;
    s.load.avg_15m           = load1 * 0.5;
    s.uptime_seconds         = 1000.0;
    return s;
}

static bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

int main() {
    // 1. Empty fold — returns false, leaves state reset.
    {
        TierAggregator agg(Level::L2);
        Sample out{};
        assert(agg.size() == 0);
        assert(agg.fold(&out) == false);
        assert(agg.size() == 0);
        assert(agg.fold(nullptr) == false);
    }

    // 2. Single-sample fold reproduces input with overridden level.
    {
        TierAggregator agg(Level::L2);
        Sample in = mk(100, Level::L3, 42.0, 0.5, 1 << 30, 25.0);
        agg.add(in);
        assert(agg.size() == 1);

        Sample out{};
        assert(agg.fold(&out) == true);
        assert(out.timestamp_nanos       == 100);
        assert(out.level                 == Level::L2);
        assert(near(out.cpu.total_percent, 42.0));
        assert(out.cpu.count             == 4);
        assert(near(out.load.avg_1m,       0.5));
        assert(out.mem.available         == (1ULL << 30));
        assert(near(out.mem.available_percent, 25.0));
        assert(agg.size() == 0);
    }

    // 3. Identical samples — mean equals the common value.
    {
        TierAggregator agg(Level::L1);
        for (int i = 0; i < 10; ++i) {
            agg.add(mk(uint64_t(i + 1) * 1000, Level::L3, 73.0, 1.25,
                       2ULL << 30, 12.5));
        }
        Sample out{};
        assert(agg.fold(&out) == true);
        assert(out.level == Level::L1);
        assert(near(out.cpu.total_percent, 73.0));
        assert(near(out.load.avg_1m,       1.25));
        assert(near(out.mem.available_percent, 12.5));
        // last-sample policy for timestamp and total
        assert(out.timestamp_nanos == 10000);
        assert(out.mem.total       == (16ULL << 30));
    }

    // 4. Varying samples — means are arithmetic averages.
    {
        TierAggregator agg(Level::L2);
        agg.add(mk(1000, Level::L3, 10.0, 0.1, 1ULL << 30,  5.0));
        agg.add(mk(2000, Level::L3, 20.0, 0.2, 2ULL << 30, 15.0));
        agg.add(mk(3000, Level::L3, 30.0, 0.3, 3ULL << 30, 25.0));

        Sample out{};
        assert(agg.fold(&out) == true);
        assert(near(out.cpu.total_percent,       20.0));   // mean of 10,20,30
        assert(near(out.load.avg_1m,              0.2));   // mean of 0.1,0.2,0.3
        assert(near(out.mem.available_percent,   15.0));   // mean of 5,15,25
        assert(out.mem.available == 2ULL << 30);           // rounded mean
        assert(out.timestamp_nanos == 3000);               // last
    }

    // 5. fold() resets; next cycle is independent.
    {
        TierAggregator agg(Level::L2);
        agg.add(mk(100, Level::L3, 50.0, 1.0, 1ULL << 30, 10.0));
        Sample out{};
        assert(agg.fold(&out) == true);
        assert(agg.size() == 0);

        agg.add(mk(200, Level::L3, 80.0, 2.0, 2ULL << 30, 20.0));
        assert(agg.fold(&out) == true);
        assert(near(out.cpu.total_percent, 80.0));
    }

    // 6. Explicit reset mid-accumulation discards pending data.
    {
        TierAggregator agg(Level::L1);
        agg.add(mk(100, Level::L3, 99.0, 10.0, 1ULL << 30, 1.0));
        agg.add(mk(200, Level::L3, 99.0, 10.0, 1ULL << 30, 1.0));
        assert(agg.size() == 2);
        agg.reset();
        assert(agg.size() == 0);
        Sample out{};
        assert(agg.fold(&out) == false);
    }

    std::printf("test_tier_aggregator: PASS\n");
    return 0;
}
