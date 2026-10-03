// SPDX-License-Identifier: BSD-3-Clause
#include "daemon/collect.h"

namespace budyk {

void collect_one(Sample* s,
                 budyk_cpu_ctx_c*  cpu_ctx,
                 budyk_disk_ctx_c* disk_ctx,
                 budyk_net_ctx_c*  net_ctx) {
    budyk_sample_c c{};
    c.timestamp_nanos = s->timestamp_nanos;

#if defined(BUDYK_LINUX)
    budyk_collect_cpu_linux    (cpu_ctx, &c);
    budyk_collect_memory_linux (&c);
    budyk_collect_uptime_linux (&c);
    budyk_collect_load_linux   (&c);
    budyk_collect_disk_linux   (disk_ctx, &c);
    budyk_collect_network_linux(net_ctx,  &c);
    budyk_collect_proc_linux   (&c);
    budyk_collect_entropy_linux(&c);
    budyk_collect_self_linux   (&c);
    budyk_collect_thermal_linux(&c);
#elif defined(BUDYK_FREEBSD)
    budyk_collect_cpu_freebsd    (cpu_ctx, &c);
    budyk_collect_memory_freebsd (&c);
    budyk_collect_uptime_freebsd (&c);
    budyk_collect_load_freebsd   (&c);
    budyk_collect_disk_freebsd   (disk_ctx, &c);
    budyk_collect_network_freebsd(net_ctx,  &c);
    budyk_collect_proc_freebsd   (&c);
    budyk_collect_entropy_freebsd(&c);
    budyk_collect_self_freebsd   (&c);
    budyk_collect_thermal_freebsd(&c);
#else
    (void)cpu_ctx; (void)disk_ctx; (void)net_ctx;
#endif

    // Copy the C-shim back into the C++ Sample (same field names, scalar
    // types match by construction in core/sample_c.h).
    s->cpu.total_percent      = c.cpu.total_percent;
    s->cpu.count              = c.cpu.count;
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
    s->entropy.available_bits  = c.entropy.available_bits;
    s->entropy.present         = (c.entropy.present != 0);
    s->self_.rss_bytes          = c.self_.rss_bytes;
    s->self_.peak_rss_bytes     = c.self_.peak_rss_bytes;
    s->self_.cpu_user_seconds   = c.self_.cpu_user_seconds;
    s->self_.cpu_system_seconds = c.self_.cpu_system_seconds;
    s->thermal.max_celsius      = c.thermal.max_celsius;
    s->thermal.sensor_count     = c.thermal.sensor_count;
    s->thermal.present          = (c.thermal.present != 0);
    s->uptime_seconds         = c.uptime_seconds;
}

} // namespace budyk
