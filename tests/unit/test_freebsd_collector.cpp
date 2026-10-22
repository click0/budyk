// SPDX-License-Identifier: BSD-3-Clause
// Exercises the FreeBSD collector C functions against the running host.
// Only added to the build when BUDYK_PLATFORM == freebsd (see tests/CMakeLists.txt).
//
// Implemented so far: CPU (kern.cp_time), memory (vm.stats.vm.* + kvm),
// uptime (kern.boottime), load (getloadavg), network (getifaddrs).
// disk is still a stub and will be added as it lands.

#include "core/sample_c.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <ctime>
#include <memory>
#include <vector>

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

namespace {

uint64_t now_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace

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

    // 8. Process count — one kern.proc.proc walk. Total > 0 on any
    //    running host (pid 1 + the test binary).
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
        // Every process is in one state; each has at least one thread.
        assert(s.proc.running + s.proc.sleeping + s.proc.blocked + s.proc.stopped +
               s.proc.zombie <= s.proc.total);
        assert(s.proc.sleeping > 0);              // init sleeps
        assert(s.proc.threads >= s.proc.total);
    }

    // 8a. The counts are per process: the total against kern.proc.proc
    //     read here (kern.proc.all has an entry per thread, and the
    //     kernel alone has dozens), the threads against the sum of its
    //     ki_numthreads (summed over per-thread entries they grew as the
    //     square); each pid once among the top processes.
    {
        size_t len = 0;
        assert(sysctlbyname("kern.proc.proc", nullptr, &len, nullptr, 0) == 0);
        std::vector<struct kinfo_proc> procs(len / sizeof(struct kinfo_proc) + 32);
        len = procs.size() * sizeof(struct kinfo_proc);
        assert(sysctlbyname("kern.proc.proc", procs.data(), &len, nullptr, 0) == 0);
        const uint64_t n = len / sizeof(struct kinfo_proc);
        uint64_t threads = 0;
        for (uint64_t i = 0; i < n; ++i) threads += static_cast<uint64_t>(procs[i].ki_numthreads);

        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        s.timestamp_nanos = now_ns();
        assert(budyk_collect_proc_freebsd_ex(&s, d.get()) == 0);
        const auto apart = [](uint64_t a, uint64_t b) { return a > b ? a - b : b - a; };
        std::printf("test_freebsd_collector: %u processes, %u threads (kern.proc.proc: %llu, %llu)\n",
                    s.proc.total, s.proc.threads,
                    static_cast<unsigned long long>(n), static_cast<unsigned long long>(threads));
        assert(apart(s.proc.total, n) <= 3 + n / 50);
        assert(apart(s.proc.threads, threads) <= 10 + threads / 50);
        for (uint32_t i = 0; i < d->nprocs; ++i) {
            for (uint32_t j = i + 1; j < d->nprocs; ++j) assert(d->procs[i].pid != d->procs[j].pid);
        }
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

    // 12. CPU breakdown: the parts add up to 100 on the second call;
    //     FreeBSD has no iowait or steal.
    {
        budyk_cpu_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_cpu_freebsd(&ctx, &s) == 0);
        volatile double spin = 0.0;
        for (int i = 0; i < 20000000; ++i) spin = spin + 1.0;
        ::usleep(50000);
        assert(budyk_collect_cpu_freebsd(&ctx, &s) == 0);
        const double sum = s.cpu.user_percent + s.cpu.system_percent + s.cpu.irq_percent +
                           s.cpu.idle_percent;
        assert(sum > 99.0 && sum < 101.0);
        assert(s.cpu.iowait_percent == 0.0 && s.cpu.steal_percent == 0.0);
        assert(std::fabs(s.cpu.total_percent - (100.0 - s.cpu.idle_percent)) < 1e-6);
    }

    // 13. TCP / UDP / ICMP from the netstat sysctls: present, rates zero on the
    //     baseline call and non-negative after.
    {
        budyk_netstat_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        s.timestamp_nanos = 1'000'000'000ULL;
        assert(budyk_collect_netstat_freebsd(&ctx, &s) == 0);
        assert(s.tcp.present && s.udp.present && s.icmp.present);
        assert(s.tcp.in_segments_per_sec == 0.0 && s.icmp.in_msgs_per_sec == 0.0);
        s.timestamp_nanos = 2'000'000'000ULL;
        assert(budyk_collect_netstat_freebsd(&ctx, &s) == 0);
        assert(s.tcp.in_segments_per_sec >= 0.0 && s.udp.in_datagrams_per_sec >= 0.0);
        assert(s.icmp.in_msgs_per_sec >= 0.0 && s.icmp.in_errors_per_sec >= 0.0);
        assert(budyk_collect_netstat_freebsd(nullptr, &s) != 0);
    }

    // 14. Filesystems: the root filesystem at least (CI VMs mount it
    //     read-write), a usage in (0, 100].
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_fs_freebsd(&s) == 0);
        assert(s.fs.present && s.fs.count >= 1);
        assert(s.fs.max_used_percent > 0.0 && s.fs.max_used_percent <= 100.0);
        assert(budyk_collect_fs_freebsd(nullptr) != 0);
    }

    // 15. Clock sync: ntp_adjtime() answers without privilege.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_clock_freebsd(&s) == 0);
        assert(s.clock.present && s.clock.max_error_ms >= 0.0);
        assert(budyk_collect_clock_freebsd(nullptr) != 0);
    }

    // 16. Per-entity detail: one entry per CPU (the count the totals
    //     report), rates after the second read; the interfaces and the
    //     local filesystems the totals counted; disks named as iostat.
    {
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        budyk_cpu_ctx_c cctx;
        std::memset(&cctx, 0, sizeof(cctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_cpu_freebsd_ex(&cctx, &s, d.get()) == 0);
        assert(d->ncores == s.cpu.count);
        ::usleep(50000);
        assert(budyk_collect_cpu_freebsd_ex(&cctx, &s, d.get()) == 0);
        for (uint32_t i = 0; i < d->ncores; ++i) {
            const double sum = d->cores[i].user_percent + d->cores[i].system_percent +
                               d->cores[i].irq_percent + d->cores[i].idle_percent;
            assert(sum == 0.0 || (sum > 99.0 && sum < 101.0));    // 0: no ticks in 50 ms
        }

        budyk_net_ctx_c nctx;
        std::memset(&nctx, 0, sizeof(nctx));
        s.timestamp_nanos = 1'000'000'000ULL;
        assert(budyk_collect_network_freebsd_ex(&nctx, &s, d.get()) == 0);
        assert(d->nifaces == s.net.interface_count && d->nifaces >= 1);
        assert(d->ifaces[0].name[0] != '\0');

        budyk_disk_ctx_c dctx;
        std::memset(&dctx, 0, sizeof(dctx));
        assert(budyk_collect_disk_freebsd_ex(&dctx, &s, d.get()) == 0);
        assert(d->ndisks == s.disk.device_count);
        for (uint32_t i = 0; i < d->ndisks; ++i) assert(d->disks[i].name[0] != '\0');

        assert(budyk_collect_fs_freebsd_ex(&s, d.get()) == 0);
        assert(d->nmounts == s.fs.count && d->nmounts >= 1);
        assert(d->mounts[0].mount[0] == '/' && d->mounts[0].total_bytes > 0);
    }

    // 17. The top processes: the second read lists some, largest CPU
    //     first, each with a pid, a name and a state letter; no system
    //     process (the kernel, idle) among them; this test itself, busy
    //     between the reads, has a CPU time.
    {
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        s.timestamp_nanos = now_ns();
        assert(budyk_collect_proc_freebsd_ex(&s, d.get()) == 0);
        volatile double spin = 0;
        const uint64_t until = now_ns() + 300000000ULL;
        while (now_ns() < until) spin = spin + 1.0;
        s.timestamp_nanos = now_ns();
        assert(budyk_collect_proc_freebsd_ex(&s, d.get()) == 0);
        assert(d->nprocs >= 1 && d->nprocs <= 2 * BUDYK_TOP_EACH);
        bool me = false;
        for (uint32_t i = 0; i < d->nprocs; ++i) {
            const budyk_process_c& p = d->procs[i];
            assert(p.pid > 0 && p.name[0] != '\0' && p.state != '\0');
            assert(std::strcmp(p.name, "idle") != 0 && std::strcmp(p.name, "kernel") != 0);
            if (i > 0) assert(d->procs[i - 1].cpu_percent >= p.cpu_percent);
            if (p.pid == getpid()) me = p.cpu_percent > 10.0;
        }
        assert(me);
    }

    std::printf("test_freebsd_collector: PASS\n");
    return 0;
}
