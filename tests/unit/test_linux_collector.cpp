// SPDX-License-Identifier: BSD-3-Clause
// Exercises the Linux collector C functions against the running host.
// Only added to the build when BUDYK_PLATFORM == linux (see tests/CMakeLists.txt).

#include "core/sample_c.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <unistd.h>

int main() {
    // 1. Memory: MemTotal / MemAvailable present and sane.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_memory_linux(&s);
        assert(rc == 0);
        assert(s.mem.total     > 0);
        assert(s.mem.available <= s.mem.total);
        assert(s.mem.available_percent >= 0.0);
        assert(s.mem.available_percent <= 100.0);
        // Swap is optional: may be zero on stripped CI containers.
        if (s.swap.total > 0) {
            assert(s.swap.used <= s.swap.total);
            assert(s.swap.used_percent >= 0.0);
            assert(s.swap.used_percent <= 100.0);
        }
    }

    // 2. Uptime: non-zero, positive.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_uptime_linux(&s);
        assert(rc == 0);
        assert(s.uptime_seconds > 0.0);
    }

    // 3. Load average: three non-negative numbers.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_load_linux(&s);
        assert(rc == 0);
        assert(s.load.avg_1m  >= 0.0);
        assert(s.load.avg_5m  >= 0.0);
        assert(s.load.avg_15m >= 0.0);
    }

    // 4. CPU delta: first call baselines (0%), second call yields a
    //    valid percentage in [0,100] and cpu.count > 0.
    {
        budyk_cpu_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));

        budyk_sample_c s1;
        std::memset(&s1, 0, sizeof(s1));
        assert(budyk_collect_cpu_linux(&ctx, &s1) == 0);
        assert(s1.cpu.count > 0);
        assert(s1.cpu.total_percent == 0.0);   // no baseline yet
        assert(ctx.has_prev == 1);

        // Burn a little CPU to guarantee the second call has a delta.
        volatile uint64_t acc = 0;
        for (uint64_t i = 0; i < 2'000'000ULL; ++i) acc += i;
        (void)acc;

        budyk_sample_c s2;
        std::memset(&s2, 0, sizeof(s2));
        assert(budyk_collect_cpu_linux(&ctx, &s2) == 0);
        assert(s2.cpu.count == s1.cpu.count);
        assert(s2.cpu.total_percent >= 0.0);
        assert(s2.cpu.total_percent <= 100.0);
    }

    // 5. Null-pointer guard.
    {
        assert(budyk_collect_memory_linux(nullptr) != 0);
        assert(budyk_collect_uptime_linux(nullptr) != 0);
        assert(budyk_collect_load_linux  (nullptr) != 0);
        budyk_cpu_ctx_c ctx{};
        budyk_sample_c  s{};
        assert(budyk_collect_cpu_linux(nullptr, &s) != 0);
        assert(budyk_collect_cpu_linux(&ctx, nullptr) != 0);

        budyk_disk_ctx_c dctx{};
        budyk_net_ctx_c  nctx{};
        assert(budyk_collect_disk_linux   (nullptr, &s) != 0);
        assert(budyk_collect_disk_linux   (&dctx, nullptr) != 0);
        assert(budyk_collect_network_linux(nullptr, &s) != 0);
        assert(budyk_collect_network_linux(&nctx, nullptr) != 0);
    }

    // Container / sandbox detection — /proc/diskstats and /proc/net/dev may
    // be absent or empty inside stripped environments (docker-scratch, some
    // CI runners). Skip the disk/net blocks in that case — they're exercised
    // on the real host in production CI and manually.
    const bool has_diskstats = (access("/proc/diskstats", R_OK) == 0);
    bool has_netdev = false;
    if (FILE* f = std::fopen("/proc/net/dev", "r")) {
        char line[8];
        has_netdev = (std::fgets(line, sizeof(line), f) != nullptr);
        std::fclose(f);
    }

    // 6. Disk: first call baselines (0 B/s); second call populates rates
    //    and preserves device_count.
    if (has_diskstats) {
        budyk_disk_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));

        budyk_sample_c s1;
        std::memset(&s1, 0, sizeof(s1));
        s1.timestamp_nanos = 1'000'000'000ULL;           // 1.0 s
        assert(budyk_collect_disk_linux(&ctx, &s1) == 0);
        assert(s1.disk.read_bytes_per_sec  == 0);
        assert(s1.disk.write_bytes_per_sec == 0);
        assert(ctx.has_prev == 1);

        // Stripped CI containers may expose zero whole disks — just assert
        // the field was populated consistently between calls.
        budyk_sample_c s2;
        std::memset(&s2, 0, sizeof(s2));
        s2.timestamp_nanos = 2'000'000'000ULL;           // dt = 1 s
        assert(budyk_collect_disk_linux(&ctx, &s2) == 0);
        assert(s2.disk.device_count == s1.disk.device_count);
        (void)s2.disk.read_bytes_per_sec;
        (void)s2.disk.write_bytes_per_sec;
    } else {
        std::printf("test_linux_collector: skip disk (no /proc/diskstats)\n");
    }

    // 7. Network: first call baselines (0 B/s); second call populates
    //    rates and preserves interface_count.
    if (has_netdev) {
        budyk_net_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));

        budyk_sample_c s1;
        std::memset(&s1, 0, sizeof(s1));
        s1.timestamp_nanos = 1'000'000'000ULL;
        assert(budyk_collect_network_linux(&ctx, &s1) == 0);
        assert(s1.net.rx_bytes_per_sec == 0);
        assert(s1.net.tx_bytes_per_sec == 0);
        assert(ctx.has_prev == 1);

        budyk_sample_c s2;
        std::memset(&s2, 0, sizeof(s2));
        s2.timestamp_nanos = 2'000'000'000ULL;
        assert(budyk_collect_network_linux(&ctx, &s2) == 0);
        assert(s2.net.interface_count == s1.net.interface_count);
        (void)s2.net.rx_bytes_per_sec;
        (void)s2.net.tx_bytes_per_sec;
    } else {
        std::printf("test_linux_collector: skip net  (empty /proc/net/dev)\n");
    }

    // 8. Disk: same timestamp on two consecutive calls must not divide
    //    by zero — collector yields zero rates instead.
    if (has_diskstats) {
        budyk_disk_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        s.timestamp_nanos = 5'000'000'000ULL;
        assert(budyk_collect_disk_linux(&ctx, &s) == 0);  // baseline

        budyk_sample_c s2;
        std::memset(&s2, 0, sizeof(s2));
        s2.timestamp_nanos = 5'000'000'000ULL;            // same ns
        assert(budyk_collect_disk_linux(&ctx, &s2) == 0);
        assert(s2.disk.read_bytes_per_sec  == 0);
        assert(s2.disk.write_bytes_per_sec == 0);
    }

    std::printf("test_linux_collector: PASS\n");
    return 0;
}
