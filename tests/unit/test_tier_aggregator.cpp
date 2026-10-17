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
    s.groups                 = kGroupsAll;
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

    // 8. The v8 fields: the CPU parts and the TCP / UDP rates are means;
    //    process states, TCP sockets, the fullest filesystem and the
    //    clock's max error are maxima; the clock offset is the one
    //    furthest from zero and synced only if every sample that read the
    //    clock was; present flags are "any"; fs.count is the last. A
    //    sample without udp leaves the udp mean to the others.
    {
        TierAggregator agg(Level::L2);
        for (int i = 0; i < 4; ++i) {
            Sample x = mk(uint64_t(i + 1) * 1000, Level::L3, 10.0, 1.0, 1ULL << 30, 10.0);
            x.cpu.user_percent    = 10.0 * (i + 1);        // 10..40 → 25
            x.cpu.iowait_percent  = i == 2 ? 8.0 : 0.0;    // → 2
            x.cpu.idle_percent    = 100.0 - x.cpu.user_percent - x.cpu.iowait_percent;
            x.proc.zombie         = i == 1 ? 5 : 0;
            x.proc.blocked        = uint32_t(i);
            x.proc.threads        = 900 + uint32_t(i);
            x.tcp.established     = i == 3 ? 7 : 40;
            x.tcp.time_wait       = uint32_t(i * 10);
            x.tcp.retransmits_per_sec = double(i);         // → 1.5
            x.tcp.present         = i != 0;
            x.udp.in_errors_per_sec = i == 0 ? 4.0 : 0.0;  // → 1
            x.udp.present         = true;
            x.fs.count            = uint32_t(3 + i);
            x.fs.max_used_percent = i == 2 ? 97.0 : 50.0;
            x.fs.max_inodes_used_percent = 20.0 + i;
            x.fs.present          = true;
            x.clock.present       = i != 3;                // the last did not read it
            x.clock.offset_ms     = i == 1 ? -9.0 : 2.0;
            x.clock.max_error_ms  = 5.0 + i;
            x.clock.synced        = i != 3;                // unsynced but not present: ignored
            x.groups              = i == 2 ? (kGroupsAll & ~kGroupUdp) : kGroupsAll;
            agg.add(x);
        }
        Sample out{};
        assert(agg.fold(&out));
        assert(near(out.cpu.user_percent, 25.0) && near(out.cpu.iowait_percent, 2.0));
        assert(near(out.cpu.idle_percent, 73.0));
        assert(out.proc.zombie == 5 && out.proc.blocked == 3 && out.proc.threads == 903);
        assert(out.tcp.established == 40 && out.tcp.time_wait == 30);
        assert(near(out.tcp.retransmits_per_sec, 1.5) && out.tcp.present);
        assert(near(out.udp.in_errors_per_sec, 4.0 / 3.0) && out.udp.present);
        assert(out.fs.count == 6 && near(out.fs.max_used_percent, 97.0));
        assert(near(out.fs.max_inodes_used_percent, 23.0) && out.fs.present);
        assert(out.clock.present && out.clock.synced);
        assert(near(out.clock.offset_ms, -9.0) && near(out.clock.max_error_ms, 7.0));
        assert(out.groups == kGroupsAll);

        // One sample that read the clock unsynced makes the window
        // unsynced; none that read it at all: not present, not synced.
        Sample y = mk(9000, Level::L3, 1.0, 1.0, 1, 1.0);
        y.clock.present = true;
        y.clock.synced  = false;
        agg.add(y);
        y.clock.synced  = true;
        agg.add(y);
        assert(agg.fold(&out) && out.clock.present && !out.clock.synced);
        agg.add(mk(9100, Level::L3, 1.0, 1.0, 1, 1.0));
        assert(agg.fold(&out) && !out.clock.present && !out.clock.synced);
    }

    // 9. Each group from the samples that have it (a level that does not
    //    collect it, a rate's first read): the mean divides by their
    //    number, the max ignores the others, the last fields come from
    //    the last that had it; the result has every group some sample
    //    had, and a group none had stays zero.
    {
        TierAggregator agg(Level::L1);
        const uint32_t minimal = kGroupCpu | kGroupMem | kGroupSwap | kGroupLoad | kGroupUptime;
        for (int i = 0; i < 4; ++i) {
            Sample x = mk(uint64_t(i + 1) * 1000, Level::L3, 10.0 * (i + 1), 1.0, 1ULL << 30, 10.0);
            x.disk.read_bytes_per_sec = 1000u * uint32_t(i + 1);   // 1000 .. 4000
            x.disk.device_count       = uint32_t(i + 1);
            x.proc.total              = 100u * uint32_t(i + 1);    // 100 .. 400
            x.net.rx_bytes_per_sec    = 77;
            x.entropy.present         = true;
            x.entropy.available_bits  = i == 3 ? 1u : 256u;
            x.uptime_seconds          = 50.0 + i;
            // disk only in samples 1 and 2, proc and entropy only in 0,
            // cpu not in 0 (its first read), net in none.
            x.groups = minimal & ~(i == 0 ? uint32_t(kGroupCpu) : 0u);
            if (i == 1 || i == 2) x.groups |= kGroupDisk;
            if (i == 0)           x.groups |= kGroupProc | kGroupEntropy;
            agg.add(x);
        }
        Sample out{};
        assert(agg.fold(&out) && out.folded && out.level == Level::L1);
        assert(out.timestamp_nanos == 4000);
        assert(near(out.cpu.total_percent, 30.0));                 // (20 + 30 + 40) / 3
        assert(out.disk.read_bytes_per_sec == 2500);               // (2000 + 3000) / 2
        assert(out.disk.device_count == 3);                        // sample 2, the last with disk
        assert(out.proc.total == 100);                             // only sample 0
        assert(out.entropy.present && out.entropy.available_bits == 256);
        assert(out.net.rx_bytes_per_sec == 0);                     // no sample had net
        assert(near(out.uptime_seconds, 53.0));
        assert(out.groups == (minimal | kGroupDisk | kGroupProc | kGroupEntropy));

        // No sample with a group at all: zero, and the means don't divide
        // by zero.
        Sample bare = mk(5000, Level::L3, 55.0, 2.0, 1, 1.0);
        bare.groups = 0;
        agg.add(bare);
        assert(agg.fold(&out) && out.groups == 0);
        assert(out.cpu.total_percent == 0.0 && out.load.avg_1m == 0.0 && out.mem.total == 0);
        assert(out.timestamp_nanos == 5000);
    }

    std::printf("test_tier_aggregator: PASS\n");
    return 0;
}
