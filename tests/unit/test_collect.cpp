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
        // A core coming online since: a baseline again, not a reading (the
        // aggregate's idle and iowait change counters with it).
        auto stat = [&](const char* text) {
            FILE* f = std::fopen((std::string(dir) + "/stat").c_str(), "w");
            assert(f != nullptr);
            std::fputs(text, f);
            std::fclose(f);
        };
        stat("cpu  20 0 20 160 0 0 0 0 0 0\ncpu0 20 0 20 160 0 0 0 0 0 0\n");
        budyk_linux_set_proc_root(dir);
        x = Sample{};
        x.timestamp_nanos = 5000000000ULL;
        collect_one(&x, &c3, set);
        assert((x.groups & kGroupCpu) != 0 && (x.baseline & kGroupCpu) == 0);
        stat("cpu  30 0 30 170 0 0 0 0 0 0\ncpu0 25 0 25 165 0 0 0 0 0 0\n"
             "cpu1 5 0 5 5 0 0 0 0 0 0\n");
        x = Sample{};
        x.timestamp_nanos = 6000000000ULL;
        collect_one(&x, &c3, set);
        budyk_linux_set_proc_root(nullptr);
        assert((x.groups & kGroupCpu) == 0 && (x.baseline & kGroupCpu) != 0);
        ::unlink((std::string(dir) + "/stat").c_str());
        ::unlink((net + "/snmp").c_str());
        ::rmdir(net.c_str());
        ::rmdir(dir);
    }

    // 7a. The readings both platforms hold to: a sensor's -273.2 °C (an
    //     ACPI zone never set, FreeBSD), +127 or -128 (an unwired Super
    //     I/O input) are none; a clock is synchronised while the NTP
    //     discipline says so and its maximum error is under 16 s
    //     (FreeBSD's kernel never says otherwise on its own: a clock
    //     whose ntpd died read synchronised for ever).
    {
        assert(budyk_celsius_plausible(45.0) && budyk_celsius_plausible(-40.0));
        assert(budyk_celsius_plausible(126.9));
        assert(!budyk_celsius_plausible(-1.0 / 10.0 - 273.15));
        assert(!budyk_celsius_plausible(127.0) && !budyk_celsius_plausible(-128.0));
        assert(!budyk_celsius_plausible(-60.0));
        assert(budyk_ntp_synced(0, 0, 15999.0));
        assert(!budyk_ntp_synced(0, 0, 16000.0));
        assert(!budyk_ntp_synced(1, 0, 1.0) && !budyk_ntp_synced(0, 1, 1.0));
        // Inodes in use and the percent: a filesystem that reports more
        // free inodes than it has in total (a FUSE or network mount, or
        // FreeBSD's signed f_ffree cast here from a negative value) gives no
        // usable count, so 0 / 0.0 — not the near 2^64 that files - ffree
        // underflows to, which drove max_inodes_used_percent off the scale.
        assert(budyk_inodes_used(1000, 100) == 900);
        assert(budyk_inodes_used_percent(1000, 100) == 90.0);
        assert(budyk_inodes_used(8, 2) == 6 && budyk_inodes_used_percent(8, 2) == 75.0);
        assert(budyk_inodes_used(1000, 1000) == 0 && budyk_inodes_used_percent(1000, 1000) == 0.0);
        assert(budyk_inodes_used(1000, 5000) == 0 && budyk_inodes_used_percent(1000, 5000) == 0.0);
        assert(budyk_inodes_used(0, 0) == 0 && budyk_inodes_used_percent(0, 50) == 0.0);
        // FreeBSD's f_ffree is int64_t; a negative value cast to uint64_t is
        // huge (> files), so it clamps to 0 rather than underflowing.
        assert(budyk_inodes_used(1000, (uint64_t)(int64_t)-1) == 0);
        assert(budyk_inodes_used_percent(1000, (uint64_t)(int64_t)-1) == 0.0);
    }

    // 7b. More devices than a list holds or the totals follow: 300 disks
    //     (a list holds 256, the totals follow 1024) and 1100 interfaces.
    //     ctx.cut says what was cut short, so it is logged once, not on
    //     every tick, and again only after it fits.
    {
        char tmpl[] = "/tmp/budyk_cutfx_XXXXXX";
        const char* dir = mkdtemp(tmpl);
        assert(dir != nullptr);
        const std::string net = std::string(dir) + "/net";
        assert(::mkdir(net.c_str(), 0700) == 0);
        auto disks = [&](unsigned n) {
            FILE* f = std::fopen((std::string(dir) + "/diskstats").c_str(), "w");
            assert(f != nullptr);
            for (unsigned i = 0; i < n; ++i) {
                std::fprintf(f, " 259 %u nvme%un1 1 0 2 0 1 0 4 0 0 1 0\n", i, i);
            }
            std::fclose(f);
        };
        auto ifaces = [&](unsigned n) {
            FILE* f = std::fopen((net + "/dev").c_str(), "w");
            assert(f != nullptr);
            std::fprintf(f, "Inter-|\n face |\n");
            for (unsigned i = 0; i < n; ++i) {
                std::fprintf(f, "  veth%u: 1000 1 0 0 0 0 0 0 10 1 0 0 0 0 0 0\n", i);
            }
            std::fclose(f);
        };
        disks(300);
        ifaces(1100);
        budyk_linux_set_proc_root(dir);
        CollectContext cx;
        tick(&cx, metric_set_full());
        assert(cx.cut == (kCutNetTotals | kDetailDisks | kDetailInterfaces));
        assert(cx.net.ndev == BUDYK_MAX_COUNTED && cx.disk.ndev == 300);
        tick(&cx, metric_set_full());
        assert(cx.cut == (kCutNetTotals | kDetailDisks | kDetailInterfaces));
        // A tick at a level that reads no lists (standard) or no disks
        // and interfaces at all (minimal) keeps what it did not check:
        // the message came again each time the level rose (L1 reads
        // standard, L2 and L3 full: each client, each escalation).
        tick(&cx, metric_set_standard());
        assert(cx.cut == (kCutNetTotals | kDetailDisks | kDetailInterfaces));
        tick(&cx, metric_set_minimal());
        assert(cx.cut == (kCutNetTotals | kDetailDisks | kDetailInterfaces));
        ifaces(2);
        tick(&cx, metric_set_full());
        assert(cx.cut == kDetailDisks);
        budyk_linux_set_proc_root(nullptr);
        ::unlink((std::string(dir) + "/diskstats").c_str());
        ::unlink((net + "/dev").c_str());
        ::rmdir(net.c_str());
        ::rmdir(dir);
    }

    // 7c. A clock step between two reads: the collectors divide by the
    //     wall-clock interval, so the rates of that tick are not readings
    //     (an NTP step at boot read every rate as 0, a VM resumed from a
    //     snapshot diluted them 3600 times). When the caller's monotonic
    //     clock (ctx.mono_now_ns) and the wall clock disagree about the
    //     interval by more than a second, the rate groups are a baseline;
    //     a tick with the clocks in step reads again. Without the
    //     monotonic clock (0) nothing changes.
    {
        char tmpl[] = "/tmp/budyk_stepfx_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        assert(dir != nullptr);
        const std::string net = std::string(dir) + "/net";
        assert(::mkdir(net.c_str(), 0700) == 0);
        auto snmp = [&](unsigned in) {
            FILE* f = std::fopen((net + "/snmp").c_str(), "w");
            assert(f != nullptr);
            std::fprintf(f, "Icmp: InMsgs InErrors InDestUnreachs OutMsgs\nIcmp: %u 0 0 0\n", in);
            std::fclose(f);
        };
        budyk_linux_set_proc_root(dir);
        CollectContext c4;
        const MetricSet set{kGroupsMinimal | kGroupIcmp, 0};
        auto tick = [&](uint64_t wall_ns, uint64_t mono_ns, unsigned in) {
            snmp(in);
            Sample x{};
            x.timestamp_nanos = wall_ns;
            c4.mono_now_ns    = mono_ns;
            collect_one(&x, &c4, set);
            return x;
        };
        Sample x = tick(1000000000ULL, 500000000000ULL, 100);
        assert((x.baseline & kGroupIcmp) != 0);
        x = tick(3000000000ULL, 502000000000ULL, 300);          // 2 s on both clocks
        assert((x.groups & kGroupIcmp) != 0 && x.icmp.in_msgs_per_sec == 100.0);
        // The wall clock stepped forward by an hour: 3602 s of wall for 2 s
        // of monotonic.
        x = tick(3605000000000ULL, 504000000000ULL, 500);
        assert((x.groups & kGroupIcmp) == 0 && (x.baseline & kGroupIcmp) != 0);
        x = tick(3607000000000ULL, 506000000000ULL, 700);       // in step again
        assert((x.groups & kGroupIcmp) != 0 && x.icmp.in_msgs_per_sec == 100.0);
        // Stepped back: the wall interval is nothing.
        x = tick(3600000000000ULL, 508000000000ULL, 900);
        assert((x.groups & kGroupIcmp) == 0 && (x.baseline & kGroupIcmp) != 0);
        // Half a second apart is the daemon's own jitter, not a step.
        x = tick(3602500000000ULL, 510000000000ULL, 1100);
        assert((x.groups & kGroupIcmp) != 0);
        // The groups that are not rates (mem, load) are readings throughout.
        assert((x.groups & kGroupLoad) != 0);
        // No monotonic clock given (a test with synthetic stamps): no check.
        CollectContext c5;
        snmp(10);
        Sample y{};
        y.timestamp_nanos = 1000000000ULL;
        collect_one(&y, &c5, set);
        snmp(30);
        y = Sample{};
        y.timestamp_nanos = 3000000000ULL;
        collect_one(&y, &c5, set);
        assert((y.groups & kGroupIcmp) != 0 && y.icmp.in_msgs_per_sec == 10.0);
        budyk_linux_set_proc_root(nullptr);
        ::unlink((net + "/snmp").c_str());
        ::rmdir(net.c_str());
        ::rmdir(dir);
    }

    // 7d. More CPUs than cpu.cores holds (BUDYK_MAX_CORES, 1024): the
    //     list is cut short and ctx.cut says so (the message came for the
    //     disks, the interfaces and the filesystems, not for the cores).
    {
        char tmpl[] = "/tmp/budyk_corefx_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        assert(dir != nullptr);
        {
            FILE* f = std::fopen((std::string(dir) + "/stat").c_str(), "w");
            assert(f != nullptr);
            std::fputs("cpu  10 0 10 80 0 0 0 0 0 0\n", f);
            for (int i = 0; i < 1030; ++i) std::fprintf(f, "cpu%d 10 0 10 80 0 0 0 0 0 0\n", i);
            std::fputs("intr 0\n", f);
            std::fclose(f);
        }
        budyk_linux_set_proc_root(dir);
        CollectContext c6;
        const MetricSet set{kGroupsMinimal, kDetailCores};
        Sample x{};
        x.timestamp_nanos = 1000000000ULL;
        collect_one(&x, &c6, set);                 // the first read: a baseline
        assert((c6.cut & kDetailCores) != 0);
        x = Sample{};
        x.timestamp_nanos = 2000000000ULL;
        collect_one(&x, &c6, set);
        budyk_linux_set_proc_root(nullptr);
        assert(x.cpu.count == 1030 && (x.groups & kGroupCpu) != 0);
        assert(c6.detail->ncores == BUDYK_MAX_CORES);
        assert((c6.cut & kDetailCores) != 0);
        ::unlink((std::string(dir) + "/stat").c_str());
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
