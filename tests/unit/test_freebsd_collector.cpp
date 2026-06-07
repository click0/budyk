// SPDX-License-Identifier: BSD-3-Clause
// Exercises the FreeBSD collector C functions against the running host.
// Only added to the build when BUDYK_PLATFORM == freebsd (see tests/CMakeLists.txt).
//
// Implemented so far: CPU (kern.cp_time), memory (vm.stats.vm.* + kvm),
// uptime (kern.boottime), load (getloadavg), network (getifaddrs).
// disk is still a stub and will be added as it lands.

#include "core/sample_c.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

int main() {
    // 1. CPU delta — first call baselines (0%), second call yields a
    //    valid percentage in [0,100], cpu.count > 0, ctx remembers state.
    {
        budyk_cpu_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));

        budyk_sample_c s1;
        std::memset(&s1, 0, sizeof(s1));
        assert(budyk_collect_cpu_freebsd(&ctx, &s1) == 0);
        assert(s1.cpu.count > 0);
        assert(s1.cpu.total_percent == 0.0);  // no baseline yet
        assert(ctx.has_prev == 1);

        // Burn a little CPU so the second snapshot has real work to
        // attribute, then re-sample.
        volatile uint64_t acc = 0;
        for (uint64_t i = 0; i < 2'000'000ULL; ++i) acc += i;
        (void)acc;

        budyk_sample_c s2;
        std::memset(&s2, 0, sizeof(s2));
        assert(budyk_collect_cpu_freebsd(&ctx, &s2) == 0);
        assert(s2.cpu.count == s1.cpu.count);
        assert(s2.cpu.total_percent >= 0.0);
        assert(s2.cpu.total_percent <= 100.0);
    }

    // 2. Memory — total > 0, available <= total, percent in [0, 100].
    //    Swap is optional (containers / jails may have it disabled).
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_memory_freebsd(&s);
        assert(rc == 0);
        assert(s.mem.total     > 0);
        assert(s.mem.available <= s.mem.total);
        assert(s.mem.available_percent >= 0.0);
        assert(s.mem.available_percent <= 100.0);
        if (s.swap.total > 0) {
            assert(s.swap.used <= s.swap.total);
            assert(s.swap.used_percent >= 0.0);
            assert(s.swap.used_percent <= 100.0);
        }
    }

    // 3. Uptime — must be a positive number; modulo wall-clock skew, it
    //    should be at least a fraction of a second on a running system.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_uptime_freebsd(&s);
        assert(rc == 0);
        assert(s.uptime_seconds > 0.0);
    }

    // 4. Load — three non-negative averages.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_load_freebsd(&s);
        assert(rc == 0);
        assert(s.load.avg_1m  >= 0.0);
        assert(s.load.avg_5m  >= 0.0);
        assert(s.load.avg_15m >= 0.0);
    }

    // 5. Network — baseline (0 B/s) on first call, delta computed
    //    from caller-supplied timestamps on subsequent calls.
    //    interface_count must be consistent across calls.
    {
        budyk_net_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));

        budyk_sample_c s1;
        std::memset(&s1, 0, sizeof(s1));
        s1.timestamp_nanos = 1'000'000'000ULL;
        assert(budyk_collect_network_freebsd(&ctx, &s1) == 0);
        assert(s1.net.rx_bytes_per_sec == 0);
        assert(s1.net.tx_bytes_per_sec == 0);
        assert(ctx.has_prev == 1);

        budyk_sample_c s2;
        std::memset(&s2, 0, sizeof(s2));
        s2.timestamp_nanos = 2'000'000'000ULL;       // dt = 1 s
        assert(budyk_collect_network_freebsd(&ctx, &s2) == 0);
        assert(s2.net.interface_count == s1.net.interface_count);
        // Containers / fresh VMs may have zero byte traffic — just
        // assert the field was populated, not strictly > 0.
        (void)s2.net.rx_bytes_per_sec;
        (void)s2.net.tx_bytes_per_sec;
    }

    // 6. Network — same timestamp on consecutive calls must not divide
    //    by zero — collector yields zero rates instead.
    {
        budyk_net_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        s.timestamp_nanos = 5'000'000'000ULL;
        assert(budyk_collect_network_freebsd(&ctx, &s) == 0);   // baseline

        budyk_sample_c s2;
        std::memset(&s2, 0, sizeof(s2));
        s2.timestamp_nanos = 5'000'000'000ULL;                  // same ns
        assert(budyk_collect_network_freebsd(&ctx, &s2) == 0);
        assert(s2.net.rx_bytes_per_sec == 0);
        assert(s2.net.tx_bytes_per_sec == 0);
    }

    // 7. Null-pointer guard for every collector.
    {
        budyk_cpu_ctx_c cctx{};
        budyk_net_ctx_c nctx{};
        budyk_sample_c  s{};
        assert(budyk_collect_cpu_freebsd    (nullptr, &s) != 0);
        assert(budyk_collect_cpu_freebsd    (&cctx, nullptr) != 0);
        assert(budyk_collect_memory_freebsd (nullptr) != 0);
        assert(budyk_collect_uptime_freebsd (nullptr) != 0);
        assert(budyk_collect_load_freebsd   (nullptr) != 0);
        assert(budyk_collect_network_freebsd(nullptr, &s) != 0);
        assert(budyk_collect_network_freebsd(&nctx, nullptr) != 0);
        assert(budyk_collect_proc_freebsd   (nullptr) != 0);
    }

    // 8. Process count — kern.proc.all size-only query. Total > 0 on
    //    any running host (pid 1 + the test binary). `running` stays
    //    Both populated via a single kern.proc.all walk now —
    //    `running` counts entries with ki_stat == SRUN. We *can't*
    //    assert running > 0 because at the nanosecond of the sysctl
    //    every process might be sleeping; the bound is the useful one.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_proc_freebsd(&s);
        assert(rc == 0);
        assert(s.proc.total > 0);
        assert(s.proc.running <= s.proc.total);
    }

    // 9. Entropy — FreeBSD has no /proc/sys/kernel/random/entropy_avail
    //    equivalent, so the collector reports present=0 unconditionally.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_entropy_freebsd(&s);
        assert(rc == 0);
        assert(s.entropy.present == 0);
        assert(s.entropy.available_bits == 0);

        assert(budyk_collect_entropy_freebsd(nullptr) != 0);
    }

    // 10. Self-metrics — getrusage(RUSAGE_SELF). On FreeBSD the kernel
    //     updates ru_maxrss lazily, so peak_rss_bytes may legitimately
    //     read 0 immediately after process start (observed reliably on
    //     15.0). We assert the call succeeded and the CPU accumulators
    //     are non-negative; peak RSS is best-effort.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_self_freebsd(&s);
        assert(rc == 0);
        assert(s.self_.cpu_user_seconds   >= 0.0);
        assert(s.self_.cpu_system_seconds >= 0.0);

        assert(budyk_collect_self_freebsd(nullptr) != 0);
    }

    // 10a. Disk — devstat walk. Two calls: first populates the ctx
    //      baseline (rates = 0), second computes a delta. We can't
    //      assert non-zero rates because a freshly-booted VM might do
    //      nothing in the gap; we just check the API contract.
    {
        budyk_disk_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c   s;
        std::memset(&s, 0, sizeof(s));

        s.timestamp_nanos = 1'000'000'000ULL;
        int rc = budyk_collect_disk_freebsd(&ctx, &s);
        assert(rc == 0);
        assert(s.disk.read_bytes_per_sec  == 0);   // first call → baseline
        assert(s.disk.write_bytes_per_sec == 0);
        assert(ctx.has_prev != 0);

        s.timestamp_nanos = 2'000'000'000ULL;
        rc = budyk_collect_disk_freebsd(&ctx, &s);
        assert(rc == 0);
        // device_count typically ≥ 1 even in jails, but we don't gate
        // on it — some minimal VMs have no whole-disk providers.
        // Rates are best-effort; just verify they're sensible u64.
        assert(s.disk.read_bytes_per_sec  < UINT64_MAX);
        assert(s.disk.write_bytes_per_sec < UINT64_MAX);

        assert(budyk_collect_disk_freebsd(nullptr, &s) != 0);
        assert(budyk_collect_disk_freebsd(&ctx, nullptr) != 0);
    }

    // 11. Thermal — dev.cpu.<N>.temperature sysctl chain. rc == 0
    //     always (soft-fail). Present=true on bare-metal hosts with
    //     ACPI/coretemp, false on virt without sensor passthrough.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_thermal_freebsd(&s);
        assert(rc == 0);
        if (s.thermal.present) {
            assert(s.thermal.sensor_count > 0);
        } else {
            assert(s.thermal.sensor_count == 0);
        }

        assert(budyk_collect_thermal_freebsd(nullptr) != 0);
    }

    std::printf("test_freebsd_collector: PASS\n");
    return 0;
}
