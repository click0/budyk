// SPDX-License-Identifier: BSD-3-Clause
#include "daemon/collect.h"

namespace budyk {

void collect_one(Sample* s, CollectContext* ctx) {
    budyk_sample_c c{};
    c.timestamp_nanos = s->timestamp_nanos;

#if defined(BUDYK_LINUX)
    budyk_collect_cpu_linux    (&ctx->cpu, &c);
    budyk_collect_memory_linux (&c);
    budyk_collect_uptime_linux (&c);
    budyk_collect_load_linux   (&c);
    budyk_collect_disk_linux   (&ctx->disk, &c);
    budyk_collect_network_linux(&ctx->net,  &c);
    budyk_collect_proc_linux   (&c);
    budyk_collect_entropy_linux(&c);
    budyk_collect_self_linux   (&c);
    budyk_collect_thermal_linux(&c);
    budyk_collect_netstat_linux(&ctx->netstat, &c);
    budyk_collect_fs_linux     (&c);
    budyk_collect_clock_linux  (&c);
#elif defined(BUDYK_FREEBSD)
    budyk_collect_cpu_freebsd    (&ctx->cpu, &c);
    budyk_collect_memory_freebsd (&c);
    budyk_collect_uptime_freebsd (&c);
    budyk_collect_load_freebsd   (&c);
    budyk_collect_disk_freebsd   (&ctx->disk, &c);
    budyk_collect_network_freebsd(&ctx->net,  &c);
    budyk_collect_proc_freebsd   (&c);
    budyk_collect_entropy_freebsd(&c);
    budyk_collect_self_freebsd   (&c);
    budyk_collect_thermal_freebsd(&c);
    budyk_collect_netstat_freebsd(&ctx->netstat, &c);
    budyk_collect_fs_freebsd     (&c);
    budyk_collect_clock_freebsd  (&c);
#else
    (void)ctx;
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
    s->fs.count                   = c.fs.count;
    s->fs.max_used_percent        = c.fs.max_used_percent;
    s->fs.max_inodes_used_percent = c.fs.max_inodes_used_percent;
    s->fs.present                 = (c.fs.present != 0);
    s->clock.offset_ms    = c.clock.offset_ms;
    s->clock.max_error_ms = c.clock.max_error_ms;
    s->clock.synced       = (c.clock.synced != 0);
    s->clock.present      = (c.clock.present != 0);
    s->uptime_seconds         = c.uptime_seconds;
    // Every collector runs on every tick for now (per-level metric sets
    // come with the collection levels' own lists). file_watch is the
    // serve loop's and is counted as collected either way.
    s->groups = kGroupsAll;
}

} // namespace budyk
