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

    // 7. Every field has its rule (see tier_aggregator.h): means for the
    //    rates, max for the process counts, temperature and RSS, min for
    //    entropy, a sum of file events, "any" for the present flags, last
    //    for counts, totals and cumulative fields. Samples without a
    //    sensor or entropy reading don't drag the extremes to 0. The
    //    result is flagged folded; first_timestamp() is the window start.
    {
        TierAggregator agg(Level::L1);
        assert(agg.first_timestamp() == 0);
        const uint64_t disk_r[3]  = {100, 200, 600};
        const uint64_t net_tx[3]  = {10, 20, 31};
        const uint32_t running[3] = {2, 9, 3};
        const double   temp[3]    = {-5.0, 0.0, -3.0};   // the 0 is "no sensor"
        const bool     tpres[3]   = {true, false, true};
        const uint32_t ent[3]     = {3000, 0, 1500};      // the 0 is "no pool"
        const bool     epres[3]   = {true, false, true};
        const uint64_t rss[3]     = {50, 80, 60};
        for (int i = 0; i < 3; ++i) {
            Sample x = mk(uint64_t(i + 1) * 1000, Level::L3, 10.0, 1.0, 1ULL << 30, 10.0);
            x.disk.read_bytes_per_sec  = disk_r[i];
            x.disk.write_bytes_per_sec = 7;
            x.disk.device_count        = uint32_t(i + 1);
            x.net.tx_bytes_per_sec     = net_tx[i];
            x.net.interface_count      = 2;
            x.proc.total               = 100 + uint32_t(i);
            x.proc.running             = running[i];
            x.thermal.max_celsius      = temp[i];
            x.thermal.present          = tpres[i];
            x.thermal.sensor_count     = tpres[i] ? 4 : 0;
            x.entropy.available_bits   = ent[i];
            x.entropy.present          = epres[i];
            x.self_.rss_bytes          = rss[i];
            x.self_.peak_rss_bytes     = 90 + uint64_t(i);
            x.self_.cpu_user_seconds   = 1.5 * (i + 1);
            x.file_watch.events_this_tick = uint32_t(i);
            x.file_watch.watched_count    = 5;
            x.file_watch.present          = i == 1;
            x.uptime_seconds              = 1000.0 + i;
            agg.add(x);
            assert(agg.first_timestamp() == 1000);
        }
        Sample out{};
        assert(agg.fold(&out));
        assert(out.folded && out.level == Level::L1 && out.timestamp_nanos == 3000);
        assert(out.disk.read_bytes_per_sec == 300 && out.disk.write_bytes_per_sec == 7);
        assert(out.disk.device_count == 3);                       // last
        assert(out.net.tx_bytes_per_sec == 20);                   // 61/3 rounded
        assert(out.net.interface_count == 2);
        assert(out.proc.total == 102 && out.proc.running == 9);   // max
        assert(out.thermal.present && near(out.thermal.max_celsius, -3.0));
        assert(out.thermal.sensor_count == 4);
        assert(out.entropy.present && out.entropy.available_bits == 1500);
        assert(out.self_.rss_bytes == 80);                        // max
        assert(out.self_.peak_rss_bytes == 92);                   // last
        assert(near(out.self_.cpu_user_seconds, 4.5));            // last
        assert(out.file_watch.events_this_tick == 3);             // 0+1+2
        assert(out.file_watch.watched_count == 5 && out.file_watch.present);
        assert(near(out.uptime_seconds, 1002.0));
        assert(agg.first_timestamp() == 0);

        // No reading in the window at all: present stays false, value 0.
        Sample y = mk(5000, Level::L3, 1.0, 1.0, 1, 1.0);
        agg.add(y);
        assert(agg.fold(&out));
        assert(!out.thermal.present && out.thermal.max_celsius == 0.0);
        assert(!out.entropy.present && out.entropy.available_bits == 0);
    }

    std::printf("test_tier_aggregator: PASS\n");
    return 0;
}
