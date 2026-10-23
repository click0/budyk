// SPDX-License-Identifier: BSD-3-Clause
// collect_one() against the running system: which collectors a metric
// set runs and what the sample then says it has. The values are the
// host's; only the groups, the lists and the zeroes are checked.
#include "core/clock.h"
#include "core/metric_set.h"
#include "core/sample.h"
#include "daemon/collect.h"

#include <cassert>
#include <cstdio>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using namespace budyk;

namespace {

constexpr uint32_t kRates = kGroupCpu | kGroupDisk | kGroupNet | kGroupTcp | kGroupUdp |
                           kGroupIcmp;

Sample tick(CollectContext* ctx, const MetricSet& set) {
    Sample s{};
    s.timestamp_nanos = now_realtime_ns();
    collect_one(&s, ctx, set);
    return s;
}

} // namespace

int main() {
    // 1. The first tick reads everything but has no rate yet: cpu, disk,
    //    net, tcp, udp and the cores, disks and interfaces lists are
    //    missing (and zero); the mounts are there. file_watch is the
    //    serve loop's to add.
    CollectContext ctx;
    Sample s = tick(&ctx, metric_set_full());
    assert(s.groups == (kGroupsSelectable & ~kRates));
    assert(s.baseline == kRates);
    assert(s.cpu.total_percent == 0.0 && s.cpu.count == 0);
    assert(s.disk.device_count == 0 && s.net.interface_count == 0 && !s.tcp.present);
    assert(s.mem.total > 0 && s.uptime_seconds > 0.0);
    assert(ctx.detail->collected == kDetailMounts);
    assert(ctx.detail->baseline ==
           (kDetailCores | kDetailDisks | kDetailInterfaces | kDetailProcesses));
    assert(ctx.detail->nprocs == 0);
    assert(ctx.detail->ncores == 0 && ctx.detail->ndisks == 0 && ctx.detail->nifaces == 0);
    assert(ctx.detail->nmounts == s.fs.count);

    // 2. The second has it all, the top processes included (this test
    //    among them: no kernel thread, a pid and a name each).
    s = tick(&ctx, metric_set_full());
    assert(s.groups == kGroupsSelectable && s.baseline == 0);
    assert(s.cpu.count >= 1 && ctx.detail->ncores == s.cpu.count);
    assert(ctx.detail->collected == kDetailAll && ctx.detail->baseline == 0);
    assert(ctx.detail->nprocs >= 1 && ctx.detail->nprocs <= 2 * BUDYK_TOP_EACH);
    for (uint32_t i = 0; i < ctx.detail->nprocs; ++i) {
        assert(ctx.detail->procs[i].pid > 0 && ctx.detail->procs[i].name[0] != '\0');
    }

    // 3. minimal: only cpu, mem, swap, load, uptime; the rest zero, no list.
    s = tick(&ctx, metric_set_minimal());
    assert(s.groups == kGroupsMinimal && s.baseline == 0);
    assert(s.cpu.count >= 1 && s.mem.total > 0);
    assert(s.proc.total == 0 && s.fs.count == 0 && !s.fs.present && s.self_.rss_bytes == 0);
    assert(ctx.detail->collected == 0);
    assert(ctx.detail->ncores == 0 && ctx.detail->nmounts == 0);

    // 4. Back to full: what minimal did not read has only its baseline
    //    (cpu did read, so it stays); the gauges are back at once.
    s = tick(&ctx, metric_set_full());
    assert(s.groups == (kGroupsSelectable & ~(kGroupDisk | kGroupNet | kGroupTcp | kGroupUdp |
                                              kGroupIcmp)));
    assert(s.baseline == (kGroupDisk | kGroupNet | kGroupTcp | kGroupUdp | kGroupIcmp));
    assert(s.disk.read_bytes_per_sec == 0 && s.disk.device_count == 0);
    assert(s.proc.total > 0);
    assert(ctx.detail->collected == kDetailMounts);
    s = tick(&ctx, metric_set_full());
    assert(s.groups == kGroupsSelectable && ctx.detail->collected == kDetailAll);

    // 5. standard: every group, no list. A list then needs a tick of its
    //    own (its rates), the mounts excepted.
    s = tick(&ctx, metric_set_standard());
    assert(s.groups == kGroupsSelectable && ctx.detail->collected == 0);
    s = tick(&ctx, metric_set_full());
    assert(s.groups == kGroupsSelectable && ctx.detail->collected == kDetailMounts);

    // 6. A list brings its group; tcp brings udp and icmp (one read).
    CollectContext c2;
    tick(&c2, MetricSet{kGroupsMinimal | kGroupTcp, kDetailDisks});
    s = tick(&c2, MetricSet{kGroupsMinimal | kGroupTcp, kDetailDisks});
    assert(s.groups == (kGroupsMinimal | kGroupTcp | kGroupUdp | kGroupIcmp | kGroupDisk));
    assert(c2.detail->collected == kDetailDisks);
    assert(c2.last_groups == s.groups && c2.last_detail == kDetailDisks);

#if defined(BUDYK_LINUX)
    // 7. What the collectors read reaches the sample: ICMP from a fixture
    //    /proc/net/snmp, every field, as rates over the 2 s between ticks.
    {
        char tmpl[] = "/tmp/budyk_collect_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        assert(dir != nullptr);
        const std::string net = std::string(dir) + "/net";
        assert(::mkdir(net.c_str(), 0700) == 0);
        auto snmp = [&](unsigned in, unsigned out, unsigned err, unsigned unr) {
            FILE* f = std::fopen((net + "/snmp").c_str(), "w");
            assert(f != nullptr);
            std::fprintf(f, "Icmp: InMsgs InErrors InDestUnreachs OutMsgs\nIcmp: %u %u %u %u\n",
                         in, err, unr, out);
            std::fclose(f);
        };
        budyk_linux_set_proc_root(dir);
        CollectContext c3;
        const MetricSet set{kGroupsMinimal | kGroupIcmp, 0};
        snmp(100, 10, 2, 4);
        Sample x{};
        x.timestamp_nanos = 1000000000ULL;
        collect_one(&x, &c3, set);
        assert((x.baseline & kGroupIcmp) != 0);
        snmp(300, 50, 6, 24);
        x = Sample{};
        x.timestamp_nanos = 3000000000ULL;
        collect_one(&x, &c3, set);
        budyk_linux_set_proc_root(nullptr);
        assert((x.groups & kGroupIcmp) != 0 && x.icmp.present);
        assert(x.icmp.in_msgs_per_sec == 100.0 && x.icmp.out_msgs_per_sec == 20.0);
        assert(x.icmp.in_errors_per_sec == 2.0 && x.icmp.in_dest_unreachs_per_sec == 10.0);
        // The fixture has no /proc/stat: the cpu collector fails, and its
        // group is left out rather than zeros passed off as a reading
        // (0 % busy). Once /proc/stat is there, cpu reads again, first as
        // a baseline.
        assert((x.groups & kGroupCpu) == 0);
        assert((c3.failed & kGroupCpu) != 0 && (x.baseline & kGroupCpu) == 0);
        {
            FILE* f = std::fopen((std::string(dir) + "/stat").c_str(), "w");
            assert(f != nullptr);
            std::fputs("cpu  10 0 10 80 0 0 0 0 0 0\ncpu0 10 0 10 80 0 0 0 0 0 0\n", f);
            std::fclose(f);
        }
        budyk_linux_set_proc_root(dir);
        x = Sample{};
        x.timestamp_nanos = 4000000000ULL;
        collect_one(&x, &c3, set);
        budyk_linux_set_proc_root(nullptr);
        assert((c3.failed & kGroupCpu) == 0 && (x.baseline & kGroupCpu) != 0);
        ::unlink((std::string(dir) + "/stat").c_str());
        ::unlink((net + "/snmp").c_str());
        ::rmdir(net.c_str());
        ::rmdir(dir);
    }
#endif

    // 8. The byte rates in double: 60 GB in a 5-minute L1 interval is
    //    200 MB/s (delta x 1e9 in 64 bits overflowed above 18.4 GB); a
    //    counter that went back, no interval or no previous read is 0;
    //    the most a rate can be is UINT64_MAX.
    {
        const uint64_t prev = 0, big = UINT64_MAX;
        assert(budyk_rate_u64(60000000000ULL, &prev, 300000000000ULL) == 200000000ULL);
        const uint64_t later = 70000000000ULL;
        assert(budyk_rate_u64(60000000000ULL, &later, 300000000000ULL) == 0);
        assert(budyk_rate_u64(60000000000ULL, &prev, 0) == 0);
        assert(budyk_rate_u64(60000000000ULL, nullptr, 1) == 0);
        assert(budyk_rate_u64(big, &prev, 1) == UINT64_MAX);
    }

    std::printf("test_collect: PASS\n");
    return 0;
}
