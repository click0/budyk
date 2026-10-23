// SPDX-License-Identifier: BSD-3-Clause
#include "daemon/collect.h"

#include <cstdio>
#include <cstring>

namespace budyk {

void collect_one(Sample* s, CollectContext* ctx, const MetricSet& set) {
    budyk_sample_c c{};
    c.timestamp_nanos = s->timestamp_nanos;

    const uint32_t lists = set.detail & kDetailAll;
    uint32_t want = set.groups | kGroupsMinimal;
    if ((lists & kDetailDisks)      != 0) want |= kGroupDisk;
    if ((lists & kDetailInterfaces) != 0) want |= kGroupNet;
    if ((lists & kDetailMounts)     != 0) want |= kGroupFs;
    if ((lists & kDetailProcesses)  != 0) want |= kGroupProc;
    // tcp, udp and icmp come from one read.
    constexpr uint32_t kNetstat = kGroupTcp | kGroupUdp | kGroupIcmp;
    if ((want & kNetstat) != 0) want |= kNetstat;

    budyk_detail_c* d = ctx->detail.get();
    d->ncores = d->ndisks = d->nifaces = d->nmounts = d->nprocs = 0;
    auto list = [&](uint32_t bit) { return (lists & bit) != 0 ? d : nullptr; };
    auto wants = [&](uint32_t group) { return (want & group) != 0; };
    // A collector that fails (-errno) leaves its groups zeroed: they are
    // not collected on this tick, rather than zeros read as a reading
    // (proc.total = 0 when kern.proc.proc failed in a fork storm).
    // Logged when it starts failing, with the reason, not on every tick.
    uint32_t failed = 0;
    auto run = [&failed, ctx](uint32_t groups, int rc) {
        if (rc >= 0) return;
        if ((ctx->failed & groups) == 0) {
            std::fprintf(stderr, "budyk: collecting %s failed: %s; left out until it reads again\n",
                         metric_group_name(groups & (0u - groups)), std::strerror(-rc));
        }
        failed |= groups;
    };

#if defined(BUDYK_LINUX)
    run(kGroupCpu, budyk_collect_cpu_linux_ex(&ctx->cpu, &c, list(kDetailCores)));
    run(kGroupMem | kGroupSwap, budyk_collect_memory_linux(&c));
    run(kGroupUptime, budyk_collect_uptime_linux(&c));
    run(kGroupLoad,   budyk_collect_load_linux(&c));
    if (wants(kGroupDisk))    run(kGroupDisk, budyk_collect_disk_linux_ex(&ctx->disk, &c, list(kDetailDisks)));
    if (wants(kGroupNet))     run(kGroupNet,  budyk_collect_network_linux_ex(&ctx->net, &c, list(kDetailInterfaces)));
    if (wants(kGroupProc))    run(kGroupProc, budyk_collect_proc_linux_ex(&c, list(kDetailProcesses)));
    if (wants(kGroupEntropy)) run(kGroupEntropy, budyk_collect_entropy_linux(&c));
    if (wants(kGroupSelf))    run(kGroupSelf,    budyk_collect_self_linux(&c));
    if (wants(kGroupThermal)) run(kGroupThermal, budyk_collect_thermal_linux(&c));
    if (wants(kGroupTcp))     run(kNetstat,      budyk_collect_netstat_linux(&ctx->netstat, &c));
    if (wants(kGroupFs))      run(kGroupFs,      budyk_collect_fs_linux_ex(&c, list(kDetailMounts)));
    if (wants(kGroupClock))   run(kGroupClock,   budyk_collect_clock_linux(&c));
#elif defined(BUDYK_FREEBSD)
    run(kGroupCpu, budyk_collect_cpu_freebsd_ex(&ctx->cpu, &c, list(kDetailCores)));
    run(kGroupMem | kGroupSwap, budyk_collect_memory_freebsd(&c));
    run(kGroupUptime, budyk_collect_uptime_freebsd(&c));
    run(kGroupLoad,   budyk_collect_load_freebsd(&c));
    if (wants(kGroupDisk))    run(kGroupDisk, budyk_collect_disk_freebsd_ex(&ctx->disk, &c, list(kDetailDisks)));
    if (wants(kGroupNet))     run(kGroupNet,  budyk_collect_network_freebsd_ex(&ctx->net, &c, list(kDetailInterfaces)));
    if (wants(kGroupProc))    run(kGroupProc, budyk_collect_proc_freebsd_ex(&c, list(kDetailProcesses)));
    if (wants(kGroupEntropy)) run(kGroupEntropy, budyk_collect_entropy_freebsd(&c));
    if (wants(kGroupSelf))    run(kGroupSelf,    budyk_collect_self_freebsd(&c));
    if (wants(kGroupThermal)) run(kGroupThermal, budyk_collect_thermal_freebsd(&c));
    if (wants(kGroupTcp))     run(kNetstat,      budyk_collect_netstat_freebsd(&ctx->netstat, &c));
    if (wants(kGroupFs))      run(kGroupFs,      budyk_collect_fs_freebsd_ex(&c, list(kDetailMounts)));
    if (wants(kGroupClock))   run(kGroupClock,   budyk_collect_clock_freebsd(&c));
#else
    (void)list;
    (void)wants;
    (void)run;
#endif

    // Copy the C-shim back into the C++ Sample (same field names, scalar
    // types match by construction in core/sample_c.h).
    s->cpu.total_percent      = c.cpu.total_percent;
    s->cpu.count              = c.cpu.count;
    s->cpu.user_percent       = c.cpu.user_percent;
    s->cpu.system_percent     = c.cpu.system_percent;
    s->cpu.iowait_percent     = c.cpu.iowait_percent;
    s->cpu.irq_percent        = c.cpu.irq_percent;
    s->cpu.steal_percent      = c.cpu.steal_percent;
    s->cpu.idle_percent       = c.cpu.idle_percent;
    s->mem.total              = c.mem.total;
    s->mem.available          = c.mem.available;
    s->mem.available_percent  = c.mem.available_percent;
    s->swap.total             = c.swap.total;
    s->swap.used              = c.swap.used;
    s->swap.used_percent      = c.swap.used_percent;
    s->load.avg_1m            = c.load.avg_1m;
    s->load.avg_5m            = c.load.avg_5m;
    s->load.avg_15m           = c.load.avg_15m;
    s->disk.read_bytes_per_sec  = c.disk.read_bytes_per_sec;
    s->disk.write_bytes_per_sec = c.disk.write_bytes_per_sec;
    s->disk.device_count        = c.disk.device_count;
    s->net.rx_bytes_per_sec     = c.net.rx_bytes_per_sec;
    s->net.tx_bytes_per_sec     = c.net.tx_bytes_per_sec;
    s->net.interface_count      = c.net.interface_count;
    s->proc.total              = c.proc.total;
    s->proc.running            = c.proc.running;
    s->proc.sleeping           = c.proc.sleeping;
    s->proc.blocked            = c.proc.blocked;
    s->proc.stopped            = c.proc.stopped;
    s->proc.zombie             = c.proc.zombie;
    s->proc.threads            = c.proc.threads;
    s->entropy.available_bits  = c.entropy.available_bits;
    s->entropy.present         = (c.entropy.present != 0);
    s->self_.rss_bytes          = c.self_.rss_bytes;
    s->self_.peak_rss_bytes     = c.self_.peak_rss_bytes;
    s->self_.cpu_user_seconds   = c.self_.cpu_user_seconds;
    s->self_.cpu_system_seconds = c.self_.cpu_system_seconds;
    s->thermal.max_celsius      = c.thermal.max_celsius;
    s->thermal.sensor_count     = c.thermal.sensor_count;
    s->thermal.present          = (c.thermal.present != 0);
    s->tcp.established           = c.tcp.established;
    s->tcp.time_wait             = c.tcp.time_wait;
    s->tcp.active_opens_per_sec  = c.tcp.active_opens_per_sec;
    s->tcp.passive_opens_per_sec = c.tcp.passive_opens_per_sec;
    s->tcp.retransmits_per_sec   = c.tcp.retransmits_per_sec;
    s->tcp.in_segments_per_sec   = c.tcp.in_segments_per_sec;
    s->tcp.out_segments_per_sec  = c.tcp.out_segments_per_sec;
    s->tcp.in_errors_per_sec     = c.tcp.in_errors_per_sec;
    s->tcp.present               = (c.tcp.present != 0);
    s->udp.in_datagrams_per_sec  = c.udp.in_datagrams_per_sec;
    s->udp.out_datagrams_per_sec = c.udp.out_datagrams_per_sec;
    s->udp.no_port_per_sec       = c.udp.no_port_per_sec;
    s->udp.in_errors_per_sec     = c.udp.in_errors_per_sec;
    s->udp.present               = (c.udp.present != 0);
    s->icmp.in_msgs_per_sec          = c.icmp.in_msgs_per_sec;
    s->icmp.out_msgs_per_sec         = c.icmp.out_msgs_per_sec;
    s->icmp.in_errors_per_sec        = c.icmp.in_errors_per_sec;
    s->icmp.in_dest_unreachs_per_sec = c.icmp.in_dest_unreachs_per_sec;
    s->icmp.present                  = (c.icmp.present != 0);
    s->fs.count                   = c.fs.count;
    s->fs.max_used_percent        = c.fs.max_used_percent;
    s->fs.max_inodes_used_percent = c.fs.max_inodes_used_percent;
    s->fs.present                 = (c.fs.present != 0);
    s->clock.offset_ms    = c.clock.offset_ms;
    s->clock.max_error_ms = c.clock.max_error_ms;
    s->clock.synced       = (c.clock.synced != 0);
    s->clock.present      = (c.clock.present != 0);
    s->uptime_seconds         = c.uptime_seconds;

    constexpr uint32_t kRateGroups = kGroupCpu | kGroupDisk | kGroupNet | kNetstat;
    constexpr uint32_t kRateLists  = kDetailCores | kDetailDisks | kDetailInterfaces |
                                     kDetailProcesses;
    // A list goes with its group's collector.
    uint32_t failed_lists = 0;
    if ((failed & kGroupCpu)  != 0) failed_lists |= kDetailCores;
    if ((failed & kGroupDisk) != 0) failed_lists |= kDetailDisks;
    if ((failed & kGroupNet)  != 0) failed_lists |= kDetailInterfaces;
    if ((failed & kGroupFs)   != 0) failed_lists |= kDetailMounts;
    if ((failed & kGroupProc) != 0) failed_lists |= kDetailProcesses;
    const uint32_t read       = want & ~failed;
    const uint32_t read_lists = lists & ~failed_lists;
    const uint32_t have       = read & ~(kRateGroups & ~ctx->last_groups);
    const uint32_t have_lists = read_lists & ~(kRateLists & ~ctx->last_detail);
    ctx->last_groups = read;
    ctx->last_detail = read_lists;
    ctx->failed      = failed;

    s->groups = kGroupsAll;
    clear_groups(s, kGroupsAll & ~have);
    s->baseline = read & ~have;
    if ((have_lists & kDetailCores)      == 0) d->ncores  = 0;
    if ((have_lists & kDetailDisks)      == 0) d->ndisks  = 0;
    if ((have_lists & kDetailInterfaces) == 0) d->nifaces = 0;
    if ((have_lists & kDetailMounts)     == 0) d->nmounts = 0;
    if ((have_lists & kDetailProcesses)  == 0) d->nprocs  = 0;
    d->collected = have_lists;
    d->baseline  = read_lists & ~have_lists;
}

} // namespace budyk
