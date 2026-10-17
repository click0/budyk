// SPDX-License-Identifier: BSD-3-Clause
#include "core/metric_set.h"
#include "core/sample.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

using namespace budyk;

static bool same(const MetricSet& a, const MetricSet& b) {
    return a.groups == b.groups && a.detail == b.detail;
}

static MetricSet parse(std::vector<std::string> names) {
    MetricSet m{};
    std::string error;
    assert(parse_metric_set(names, &m, &error) == 0 && error.empty());
    return m;
}

int main() {
    // 1. The named sets: minimal is spec §3.4.1's L1 set; standard every
    //    group but file_watch (the serve loop's), no lists; full adds them.
    {
        assert(same(metric_set_minimal(), MetricSet{kGroupsMinimal, 0}));
        assert(kGroupsMinimal == (kGroupCpu | kGroupMem | kGroupSwap | kGroupLoad | kGroupUptime));
        assert(same(metric_set_standard(), MetricSet{kGroupsAll & ~uint32_t(kGroupFileWatch), 0}));
        assert(same(metric_set_full(), MetricSet{kGroupsSelectable, kDetailAll}));
        assert(same(MetricSet{}, metric_set_full()));
        assert(same(parse({"minimal"}),  metric_set_minimal()));
        assert(same(parse({"standard"}), metric_set_standard()));
        assert(same(parse({"full"}),     metric_set_full()));
        assert(same(parse({}),           metric_set_minimal()));
    }

    // 2. A list: the union, always with the minimal set; a list brings its
    //    group; set names mix in.
    {
        assert(same(parse({"disk"}), MetricSet{kGroupsMinimal | kGroupDisk, 0}));
        assert(same(parse({"disk", "tcp", "self"}),
                    MetricSet{kGroupsMinimal | kGroupDisk | kGroupTcp | kGroupSelf, 0}));
        assert(same(parse({"net.interfaces"}),
                    MetricSet{kGroupsMinimal | kGroupNet, kDetailInterfaces}));
        assert(same(parse({"cpu.cores", "disk.devices", "fs.mounts"}),
                    MetricSet{kGroupsMinimal | kGroupDisk | kGroupFs,
                              kDetailCores | kDetailDisks | kDetailMounts}));
        assert(same(parse({"standard", "cpu.cores"}),
                    MetricSet{kGroupsSelectable, kDetailCores}));
        assert(same(parse({"minimal", "clock", "clock"}),
                    MetricSet{kGroupsMinimal | kGroupClock, 0}));
        const char* groups[] = { "cpu", "mem", "swap", "load", "uptime", "disk", "net", "proc",
                                 "entropy", "self", "thermal", "tcp", "udp", "fs", "clock" };
        std::vector<std::string> every(std::begin(groups), std::end(groups));
        assert(same(parse(every), metric_set_standard()));
    }

    // 3. An unknown name (a typo, the wrong case, file_watch, the Lua
    //    spelling self_) is -EINVAL naming it; *out is left alone.
    {
        for (const char* bad : { "dsik", "Disk", "file_watch", "self_", "", "cpu.core", "maximal" }) {
            MetricSet m{kGroupCpu, kDetailDisks};
            std::string error;
            assert(parse_metric_set({"disk", bad}, &m, &error) == -EINVAL);
            assert(error == std::string("unknown metric '") + bad + "'");
            assert(same(m, MetricSet{kGroupCpu, kDetailDisks}));
        }
        assert(parse_metric_set({"bogus"}, nullptr, nullptr) == -EINVAL);
        assert(parse_metric_set({"disk"}, nullptr, nullptr) == 0);
    }

    // 4. Names and bits, both ways; names in bit order, groups first.
    {
        assert(std::string(metric_group_name(kGroupCpu)) == "cpu");
        assert(std::string(metric_group_name(kGroupSelf)) == "self");
        assert(std::string(metric_group_name(kGroupFileWatch)) == "file_watch");
        assert(std::string(metric_group_name(kGroupClock)) == "clock");
        assert(metric_group_name(0) == nullptr);
        assert(metric_group_name(kGroupCpu | kGroupMem) == nullptr);
        assert(metric_group_name(1u << 16) == nullptr);
        assert(std::string(metric_detail_name(kDetailMounts)) == "fs.mounts");
        assert(metric_detail_name(0) == nullptr && metric_detail_name(1u << 4) == nullptr);
        for (uint32_t bit = 1; bit <= kGroupsAll; bit <<= 1) {
            assert(metric_group_bit(metric_group_name(bit)) == bit);
        }
        for (uint32_t bit = 1; bit <= kDetailAll; bit <<= 1) {
            assert(metric_detail_bit(metric_detail_name(bit)) == bit);
        }
        assert(metric_group_bit("nope") == 0 && metric_group_bit(nullptr) == 0);
        assert(metric_detail_bit("disk") == 0 && metric_detail_bit(nullptr) == 0);

        const std::vector<std::string> min = metric_set_names(metric_set_minimal());
        assert((min == std::vector<std::string>{"cpu", "mem", "swap", "load", "uptime"}));
        const std::vector<std::string> mix =
            metric_set_names(MetricSet{kGroupCpu | kGroupClock, kDetailCores | kDetailMounts});
        assert((mix == std::vector<std::string>{"cpu", "clock", "cpu.cores", "fs.mounts"}));
        assert(metric_set_names(MetricSet{0, 0}).empty());
        // What metric_set_names() lists parses back to the same set.
        for (const MetricSet& m : { metric_set_minimal(), metric_set_standard(), metric_set_full(),
                                    parse({"net.interfaces", "proc"}) }) {
            assert(same(parse(metric_set_names(m)), m));
        }
    }

    // 5. clear_groups() zeroes exactly the groups named, and drops them
    //    from `groups`; the rest, and the fields outside any group, stay.
    {
        Sample full{};
        full.timestamp_nanos = 7; full.level = Level::L3; full.groups = kGroupsAll;
        full.cpu = CpuStats{50, 4, 1, 2, 3, 4, 5, 6};
        full.mem = MemStats{1, 2, 3};          full.swap = SwapStats{4, 5, 6};
        full.load = LoadStats{1, 2, 3};
        full.disk.read_bytes_per_sec = 9;      full.net.rx_bytes_per_sec = 9;
        full.proc.total = 9;                   full.entropy.available_bits = 9;
        full.self_.rss_bytes = 9;              full.thermal.max_celsius = 9;
        full.file_watch.watched_count = 9;     full.uptime_seconds = 9;
        full.tcp.established = 9;              full.udp.in_datagrams_per_sec = 9;
        full.fs.count = 9;                     full.clock.offset_ms = 9;
        // A value per group, as a number, to compare before and after.
        auto value = [](const Sample& s, uint32_t bit) -> double {
            switch (bit) {
                case kGroupCpu:       return s.cpu.total_percent + s.cpu.count + s.cpu.idle_percent;
                case kGroupMem:       return double(s.mem.total + s.mem.available) + s.mem.available_percent;
                case kGroupSwap:      return double(s.swap.total + s.swap.used) + s.swap.used_percent;
                case kGroupLoad:      return s.load.avg_1m + s.load.avg_5m + s.load.avg_15m;
                case kGroupDisk:      return double(s.disk.read_bytes_per_sec);
                case kGroupNet:       return double(s.net.rx_bytes_per_sec);
                case kGroupProc:      return s.proc.total;
                case kGroupEntropy:   return s.entropy.available_bits;
                case kGroupSelf:      return double(s.self_.rss_bytes);
                case kGroupThermal:   return s.thermal.max_celsius;
                case kGroupFileWatch: return s.file_watch.watched_count;
                case kGroupUptime:    return s.uptime_seconds;
                case kGroupTcp:       return s.tcp.established;
                case kGroupUdp:       return s.udp.in_datagrams_per_sec;
                case kGroupFs:        return s.fs.count;
                case kGroupClock:     return s.clock.offset_ms;
            }
            return -1;
        };
        for (uint32_t bit = 1; bit <= kGroupsAll; bit <<= 1) {
            Sample s = full;
            clear_groups(&s, bit);
            assert(s.groups == (kGroupsAll & ~bit));
            assert(s.timestamp_nanos == 7 && s.level == Level::L3);
            for (uint32_t other = 1; other <= kGroupsAll; other <<= 1) {
                assert(value(s, other) == (other == bit ? 0.0 : value(full, other)));
            }
        }
        Sample s = full;
        clear_groups(&s, kGroupsAll);
        assert(s.groups == 0 && s.mem.total == 0 && s.clock.offset_ms == 0.0);
        clear_groups(nullptr, kGroupsAll);
    }

    std::printf("test_metric_set: PASS\n");
    return 0;
}
