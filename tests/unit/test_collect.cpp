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

using namespace budyk;

namespace {

constexpr uint32_t kRates = kGroupCpu | kGroupDisk | kGroupNet | kGroupTcp | kGroupUdp;

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
    assert(ctx.detail->baseline == (kDetailCores | kDetailDisks | kDetailInterfaces));
    assert(ctx.detail->ncores == 0 && ctx.detail->ndisks == 0 && ctx.detail->nifaces == 0);
    assert(ctx.detail->nmounts == s.fs.count);

    // 2. The second has it all.
    s = tick(&ctx, metric_set_full());
    assert(s.groups == kGroupsSelectable && s.baseline == 0);
    assert(s.cpu.count >= 1 && ctx.detail->ncores == s.cpu.count);
    assert(ctx.detail->collected == kDetailAll && ctx.detail->baseline == 0);

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
    assert(s.groups == (kGroupsSelectable & ~(kGroupDisk | kGroupNet | kGroupTcp | kGroupUdp)));
    assert(s.baseline == (kGroupDisk | kGroupNet | kGroupTcp | kGroupUdp));
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

    // 6. A list brings its group; tcp brings udp (one read).
    CollectContext c2;
    tick(&c2, MetricSet{kGroupsMinimal | kGroupTcp, kDetailDisks});
    s = tick(&c2, MetricSet{kGroupsMinimal | kGroupTcp, kDetailDisks});
    assert(s.groups == (kGroupsMinimal | kGroupTcp | kGroupUdp | kGroupDisk));
    assert(c2.detail->collected == kDetailDisks);
    assert(c2.last_groups == s.groups && c2.last_detail == kDetailDisks);

    std::printf("test_collect: PASS\n");
    return 0;
}
