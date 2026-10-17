// SPDX-License-Identifier: BSD-3-Clause
// Exercises the Linux collector C functions against the running host.
// Only added to the build when BUDYK_PLATFORM == linux (see tests/CMakeLists.txt).

#include "core/sample_c.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {

// A fixture tree that stands in for /proc (budyk_linux_set_proc_root).
struct Fixture {
    std::string root;
    Fixture() {
        char tmpl[] = "/tmp/budyk_procfx_XXXXXX";
        const char* d = mkdtemp(tmpl);
        assert(d != nullptr);
        root = d;
    }
    ~Fixture() {
        budyk_linux_set_proc_root(nullptr);
        const std::string cmd = "rm -rf " + root;
        if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup of %s failed\n", root.c_str());
    }
    void file(const std::string& rel, const std::string& body) const {
        const std::string path = root + "/" + rel;
        for (size_t i = root.size() + 1; i < path.size(); ++i) {
            if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
        }
        FILE* f = std::fopen(path.c_str(), "w");
        assert(f != nullptr);
        std::fputs(body.c_str(), f);
        std::fclose(f);
    }
};

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

} // namespace

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
        assert(budyk_collect_proc_linux   (nullptr) != 0);
        assert(budyk_collect_entropy_linux(nullptr) != 0);
        assert(budyk_collect_self_linux   (nullptr) != 0);
        assert(budyk_collect_thermal_linux(nullptr) != 0);
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

    // 7b. Processes by state on this host: the states add up to total,
    //     this test process is one of them (running), and every process
    //     has at least one thread.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_proc_linux(&s) == 0);
        assert(s.proc.total > 0 && s.proc.running >= 1);
        assert(s.proc.running + s.proc.sleeping + s.proc.blocked + s.proc.stopped +
               s.proc.zombie == s.proc.total);
        assert(s.proc.threads >= s.proc.total);
    }

    // 7f. The /proc/<pid>/stat parser: comm may hold spaces and ')';
    //     num_threads is field 20. A truncated line is refused.
    {
        char st = 0;
        uint32_t th = 0;
        assert(budyk_linux_parse_proc_stat(
            "42 (my (odd) proc) S 1 42 42 0 -1 4194560 100 0 0 0 5 3 0 0 20 0 7 0 123 0\n",
            &st, &th) == 0);
        assert(st == 'S' && th == 7);
        assert(budyk_linux_parse_proc_stat("1 (init) R 0 1 1 0 -1", &st, &th) != 0);
        assert(budyk_linux_parse_proc_stat("no parens here", &st, &th) != 0);
    }

    // 7g. Processes against a fixture /proc: one of each state (I and P
    //     count as sleeping, t as stopped), X not counted, a directory
    //     that is not a pid skipped, threads summed.
    {
        Fixture fx;
        const char* tail = " 1 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 %d 0 1 0\n";
        const char states[] = {'R', 'S', 'I', 'D', 'T', 't', 'Z', 'P', 'X'};
        for (int i = 0; i < 9; ++i) {
            char line[256];
            std::snprintf(line, sizeof(line), "%d (p%d) %c", 100 + i, i, states[i]);
            char rest[128];
            std::snprintf(rest, sizeof(rest), tail, i + 1);
            fx.file(std::to_string(100 + i) + "/stat", std::string(line) + rest);
        }
        fx.file("self/stat", "1 (self) R 1 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 1 0\n");
        budyk_linux_set_proc_root(fx.root.c_str());
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_proc_linux(&s) == 0);
        assert(s.proc.total == 8 && s.proc.running == 1 && s.proc.sleeping == 3);
        assert(s.proc.blocked == 1 && s.proc.stopped == 2 && s.proc.zombie == 1);
        assert(s.proc.threads == 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8);
    }

    // 7h. TCP / UDP against a fixture: the snmp columns are looked up by
    //     name (here in an order no kernel uses), the rates come from the
    //     delta over the elapsed time, TIME_WAIT from sockstat.
    {
        Fixture fx;
        auto snmp = [&](unsigned act, unsigned estab, unsigned retr, unsigned udp_in) {
            char buf[1024];
            std::snprintf(buf, sizeof(buf),
                "Ip: Forwarding DefaultTTL\nIp: 1 64\n"
                "Tcp: CurrEstab ActiveOpens PassiveOpens InSegs OutSegs RetransSegs InErrs\n"
                "Tcp: %u %u 5 1000 900 %u 2\n"
                "Udp: InDatagrams NoPorts InErrors OutDatagrams\n"
                "Udp: %u 3 1 40\n", estab, act, retr, udp_in);
            fx.file("net/snmp", buf);
        };
        fx.file("net/sockstat", "sockets: used 100\nTCP: inuse 5 orphan 0 tw 17 alloc 6 mem 1\n");
        budyk_linux_set_proc_root(fx.root.c_str());
        budyk_netstat_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        snmp(10, 4, 50, 100);
        s.timestamp_nanos = 1'000'000'000ULL;
        assert(budyk_collect_netstat_linux(&ctx, &s) == 0);
        assert(s.tcp.present && s.udp.present);
        assert(s.tcp.established == 4 && s.tcp.time_wait == 17);
        assert(s.tcp.active_opens_per_sec == 0.0);         // baseline
        snmp(30, 6, 54, 300);
        s.timestamp_nanos = 3'000'000'000ULL;              // 2 s later
        assert(budyk_collect_netstat_linux(&ctx, &s) == 0);
        assert(s.tcp.established == 6);
        assert(near(s.tcp.active_opens_per_sec, 10.0));     // 20 in 2 s
        assert(near(s.tcp.retransmits_per_sec, 2.0));
        assert(near(s.udp.in_datagrams_per_sec, 100.0));
        assert(near(s.tcp.passive_opens_per_sec, 0.0));
        snmp(5, 6, 54, 300);                                // a counter went back
        s.timestamp_nanos = 4'000'000'000ULL;
        assert(budyk_collect_netstat_linux(&ctx, &s) == 0);
        assert(s.tcp.active_opens_per_sec == 0.0);

        fx.file("net/snmp", "Ip: Forwarding\nIp: 1\n");   // no Tcp/Udp lines
        assert(budyk_collect_netstat_linux(&ctx, &s) != 0);
        assert(!s.tcp.present && !s.udp.present);
    }

    // 7i. TCP / UDP and clock sync on this host.
    {
        budyk_netstat_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        s.timestamp_nanos = 1;
        assert(budyk_collect_netstat_linux(&ctx, &s) == 0);
        assert(s.tcp.present && s.udp.present);
        assert(budyk_collect_clock_linux(&s) == 0);
        assert(s.clock.present && s.clock.max_error_ms >= 0.0);
    }

    // 7j. Filesystems: on this host at least the one holding /tmp is
    //     counted. Against a fixture mounts file whose entries point at
    //     real, distinct filesystems (the one under /tmp, /dev, /dev/shm),
    //     so each rule alone decides the count: pseudo, network, FUSE and
    //     read-only mounts are skipped, a second mount of the same
    //     filesystem counts once, an escaped mount point (\040) is
    //     unescaped, a mount point that is gone is skipped.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_fs_linux(&s) == 0);
        assert(s.fs.present && s.fs.count >= 1);
        assert(s.fs.max_used_percent > 0.0 && s.fs.max_used_percent <= 100.0);

        char m[] = "/a\\040b\\011c\\134d";
        budyk_linux_unescape_mount(m);
        assert(std::strcmp(m, "/a b\tc\\d") == 0);

        Fixture fx;
        const std::string spaced = fx.root + "/with space";
        assert(::mkdir(spaced.c_str(), 0700) == 0);
        const bool shm = ::access("/dev/shm", F_OK) == 0;
        fx.file("self/mounts",
            "proc /proc proc rw 0 0\n"
            "sysfs /sys sysfs rw 0 0\n"
            "/dev/sdz1 " + fx.root + "/with\\040space ext4 rw 0 0\n"   // counted
            "/dev/sdz3 /dev ext4 ro 0 0\n"                    // read-only
            "server:/x /dev nfs4 rw 0 0\n"                    // network
            "sshfs#u@h: /dev fuse.sshfs rw 0 0\n"             // FUSE
            "/dev/loop0 /dev squashfs rw 0 0\n"               // an image
            "tmpfs /dev/shm tmpfs rw,nosuid 0 0\n"            // counted
            "tmpfs /dev/shm tmpfs rw 0 0\n"                   // the same again
            "/dev/sdz9 /nonexistent-mount ext4 rw 0 0\n");    // gone
        budyk_linux_set_proc_root(fx.root.c_str());
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_fs_linux(&s) == 0);
        assert(s.fs.present);
        assert(s.fs.count == (shm ? 2u : 1u));
        budyk_linux_set_proc_root(nullptr);
    }

    // 7c. Entropy — /proc/sys/kernel/random/entropy_avail. On real
    //     hosts this is a non-negative integer; on stripped containers
    //     the file may be missing, in which case present=0 and
    //     available_bits=0.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_entropy_linux(&s);
        assert(rc == 0);              /* never fails — soft-falls to present=0 */
        if (s.entropy.present) {
            (void)s.entropy.available_bits; /* any non-negative value is OK */
        } else {
            assert(s.entropy.available_bits == 0);
        }
    }

    // 7d. Self-metrics — getrusage on this very test process.
    //     peak_rss_bytes > 0 always (we've been using memory).
    //     CPU times are non-negative; rss_bytes may be 0 on stripped
    //     containers without /proc/self/statm.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_self_linux(&s);
        assert(rc == 0);
        assert(s.self_.peak_rss_bytes > 0);
        assert(s.self_.cpu_user_seconds   >= 0.0);
        assert(s.self_.cpu_system_seconds >= 0.0);
    }

    // 7e. Thermal — /sys/class/thermal/thermal_zone*/temp.
    //     rc == 0 always (never fails — soft-falls to present=0).
    //     If sensors are present, sensor_count > 0 and max_celsius is
    //     a finite reading. CI runners typically have at least one
    //     thermal_zone (acpitz) but bare containers may have none.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_thermal_linux(&s);
        assert(rc == 0);
        if (s.thermal.present) {
            assert(s.thermal.sensor_count > 0);
        } else {
            assert(s.thermal.sensor_count == 0);
            assert(s.thermal.max_celsius  == 0.0);
        }
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

    // 8b. CPU against a fixture /proc/stat: the first read is the
    //     baseline, the second gives each part's share of the delta
    //     (nice in user, softirq in irq) and total = all but idle and
    //     iowait; cpuN lines count the cores.
    {
        Fixture fx;
        auto stat = [&](unsigned user, unsigned sys, unsigned idle, unsigned iowait,
                        unsigned irq, unsigned softirq, unsigned steal) {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "cpu  %u 10 %u %u %u %u %u %u 0 0\n"
                "cpu0 1 0 1 1 0 0 0 0 0 0\ncpu1 1 0 1 1 0 0 0 0 0 0\n"
                "intr 12345\nctxt 1\n", user, sys, idle, iowait, irq, softirq, steal);
            fx.file("stat", buf);
        };
        budyk_linux_set_proc_root(fx.root.c_str());
        budyk_cpu_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        stat(1000, 500, 3000, 100, 20, 30, 0);
        assert(budyk_collect_cpu_linux(&ctx, &s) == 0);
        assert(s.cpu.count == 2 && s.cpu.total_percent == 0.0);
        // Deltas: user 100, sys 50, idle 300, iowait 40, irq 5 + softirq 5,
        // steal 0 (nice 0): 500 in all.
        stat(1100, 550, 3300, 140, 25, 35, 0);
        assert(budyk_collect_cpu_linux(&ctx, &s) == 0);
        assert(near(s.cpu.user_percent, 20.0) && near(s.cpu.system_percent, 10.0));
        assert(near(s.cpu.iowait_percent, 8.0) && near(s.cpu.irq_percent, 2.0));
        assert(near(s.cpu.steal_percent, 0.0) && near(s.cpu.idle_percent, 60.0));
        assert(near(s.cpu.total_percent, 32.0));
        budyk_linux_set_proc_root(nullptr);
    }

    // 8c. Per-entity detail against fixtures: each core from its cpuN
    //     line; each whole disk from /proc/diskstats (partitions and
    //     loop devices left out) with byte and operation rates and the
    //     busy share from io_ticks; each interface from /proc/net/dev.
    //     A device that appears gets a baseline, not a rate; one that
    //     disappears is gone from the list and from the previous
    //     counters. Without a detail struct nothing is filled.
    {
        Fixture fx;
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        budyk_linux_set_proc_root(fx.root.c_str());

        fx.file("stat", "cpu  20 0 20 160 0 0 0 0 0 0\n"
                        "cpu0 10 0 10 80 0 0 0 0 0 0\ncpu1 10 0 10 80 0 0 0 0 0 0\n");
        budyk_cpu_ctx_c cctx;
        std::memset(&cctx, 0, sizeof(cctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_cpu_linux_ex(&cctx, &s, d.get()) == 0);
        assert(d->ncores == 2 && d->cores[0].total_percent == 0.0);   // baseline
        // cpu0: 90 user; cpu1: 60 idle, 30 iowait, 10 system.
        fx.file("stat", "cpu  110 0 30 220 30 0 0 0 0 0\n"
                        "cpu0 100 0 10 80 0 0 0 0 0 0\ncpu1 10 0 20 140 30 0 0 0 0 0\n");
        assert(budyk_collect_cpu_linux_ex(&cctx, &s, d.get()) == 0);
        assert(d->ncores == 2);
        assert(near(d->cores[0].user_percent, 100.0) && near(d->cores[0].total_percent, 100.0));
        assert(near(d->cores[1].idle_percent, 60.0) && near(d->cores[1].iowait_percent, 30.0));
        assert(near(d->cores[1].total_percent, 10.0));            // not idle, not iowait

        auto diskstats = [&](unsigned rs, unsigned ws, unsigned r, unsigned w, unsigned ticks,
                             bool with_sdb) {
            char buf[1024];
            int n = std::snprintf(buf, sizeof(buf),
                "   8       0 sda %u 0 %u 0 %u 0 %u 0 0 %u 0\n"
                "   8       1 sda1 %u 0 %u 0 %u 0 %u 0 0 %u 0\n"
                "   7       0 loop0 5 0 5 0 5 0 5 0 0 5 0\n", r, rs, w, ws, ticks, r, rs, w, ws, ticks);
            if (with_sdb) {
                std::snprintf(buf + n, sizeof(buf) - size_t(n), "   8      16 sdb 1 0 8 0 1 0 8 0 0 1 0\n");
            }
            fx.file("diskstats", buf);
        };
        budyk_disk_ctx_c dctx;
        std::memset(&dctx, 0, sizeof(dctx));
        s.timestamp_nanos = 10'000'000'000ULL;
        diskstats(1000, 2000, 10, 20, 100, false);
        assert(budyk_collect_disk_linux_ex(&dctx, &s, d.get()) == 0);
        assert(d->ndisks == 1 && std::strcmp(d->disks[0].name, "sda") == 0);
        s.timestamp_nanos = 12'000'000'000ULL;                         // 2 s later
        diskstats(3000, 2000, 30, 20, 1100, true);
        assert(budyk_collect_disk_linux_ex(&dctx, &s, d.get()) == 0);
        assert(d->ndisks == 2);
        assert(d->disks[0].read_bytes_per_sec == 2000 * 512 / 2);      // 1000 sectors/s
        assert(d->disks[0].write_bytes_per_sec == 0);
        assert(near(d->disks[0].reads_per_sec, 10.0));
        assert(near(d->disks[0].busy_percent, 50.0));                  // 1000 ms of 2000
        assert(std::strcmp(d->disks[1].name, "sdb") == 0 && d->disks[1].read_bytes_per_sec == 0);
        // The totals sum every disk's counters, so a disk that appears
        // adds all of its counters once (sdb's 8 sectors here); the
        // per-device rates start it at a baseline instead.
        assert(s.disk.read_bytes_per_sec == (2000 + 8) * 512 / 2);
        s.timestamp_nanos = 13'000'000'000ULL;
        diskstats(3000, 2000, 30, 20, 1100, false);                     // sdb gone
        assert(budyk_collect_disk_linux_ex(&dctx, &s, d.get()) == 0);
        assert(d->ndisks == 1 && d->ndisk_prev == 1);

        auto netdev = [&](unsigned rx, unsigned tx, unsigned rxerr) {
            char buf[1024];
            std::snprintf(buf, sizeof(buf),
                "Inter-|   Receive  |  Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n"
                "    lo: 999 9 0 0 0 0 0 0 999 9 0 0 0 0 0 0\n"
                "  eth0: %u 10 %u 1 0 0 0 0 %u 20 0 0 0 0 0 0\n", rx, rxerr, tx);
            fx.file("net/dev", buf);
        };
        budyk_net_ctx_c nctx;
        std::memset(&nctx, 0, sizeof(nctx));
        s.timestamp_nanos = 20'000'000'000ULL;
        netdev(1000, 500, 0);
        assert(budyk_collect_network_linux_ex(&nctx, &s, d.get()) == 0);
        assert(d->nifaces == 1 && std::strcmp(d->ifaces[0].name, "eth0") == 0);
        s.timestamp_nanos = 21'000'000'000ULL;
        netdev(5000, 700, 3);
        assert(budyk_collect_network_linux_ex(&nctx, &s, d.get()) == 0);
        assert(d->ifaces[0].rx_bytes_per_sec == 4000 && d->ifaces[0].tx_bytes_per_sec == 200);
        assert(near(d->ifaces[0].rx_errors_per_sec, 3.0));
        s.timestamp_nanos = 22'000'000'000ULL;
        netdev(100, 900, 3);                    // the interface was reset: rx went back
        assert(budyk_collect_network_linux_ex(&nctx, &s, d.get()) == 0);
        assert(d->ifaces[0].rx_bytes_per_sec == 0 && d->ifaces[0].tx_bytes_per_sec == 200);

        budyk_cpu_ctx_c c2;
        std::memset(&c2, 0, sizeof(c2));
        assert(budyk_collect_cpu_linux_ex(&c2, &s, nullptr) == 0);     // no detail: fine
        budyk_linux_set_proc_root(nullptr);
    }

    // 8d. Filesystems with detail on this host: one entry per counted
    //     filesystem, with sizes that add up.
    {
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_fs_linux_ex(&s, d.get()) == 0);
        assert(d->nmounts == s.fs.count && d->nmounts >= 1);
        for (uint32_t i = 0; i < d->nmounts; ++i) {
            const budyk_mount_c& m = d->mounts[i];
            assert(m.mount[0] == '/' && m.type[0] != '\0');
            assert(m.used_bytes <= m.total_bytes && m.used_percent <= s.fs.max_used_percent + 1e-9);
        }
    }

    // 9. CPU: the parts add up to 100 on the second call, and
    //    total_percent agrees with them.
    {
        budyk_cpu_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_cpu_linux(&ctx, &s) == 0);
        volatile double spin = 0.0;
        for (int i = 0; i < 20000000; ++i) spin = spin + 1.0;
        ::usleep(20000);
        assert(budyk_collect_cpu_linux(&ctx, &s) == 0);
        const double sum = s.cpu.user_percent + s.cpu.system_percent + s.cpu.iowait_percent +
                           s.cpu.irq_percent + s.cpu.steal_percent + s.cpu.idle_percent;
        assert(sum > 99.0 && sum < 101.0);
        // total_percent is everything but idle and iowait.
        assert(near(s.cpu.total_percent, 100.0 - s.cpu.idle_percent - s.cpu.iowait_percent));
    }

    std::printf("test_linux_collector: PASS\n");
    return 0;
}
