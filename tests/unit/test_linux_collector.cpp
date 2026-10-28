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

// A fixture tree that stands in for /proc (budyk_linux_set_proc_root),
// and its sys/ for /sys: no interface is a device there unless a test
// says so, whatever this host's own are.
struct Fixture {
    std::string root;
    Fixture() {
        char tmpl[] = "/tmp/budyk_procfx_XXXXXX";
        const char* d = mkdtemp(tmpl);
        assert(d != nullptr);
        root = d;
        budyk_linux_set_sys_root((root + "/sys").c_str());
    }
    ~Fixture() {
        budyk_linux_set_proc_root(nullptr);
        budyk_linux_set_sys_root(nullptr);
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
    //     num_threads is field 20. A line cut before it is refused; one
    //     cut after it reads 0 for the rest. The whole line: pid, comm
    //     (at most 31 bytes kept), flags, utime + stime, starttime, rss.
    {
        char st = 0;
        uint32_t th = 0;
        assert(budyk_linux_parse_proc_stat(
            "42 (my (odd) proc) S 1 42 42 0 -1 4194560 100 0 0 0 5 3 0 0 20 0 7 0 123 0\n",
            &st, &th) == 0);
        assert(st == 'S' && th == 7);
        assert(budyk_linux_parse_proc_stat("1 (init) R 0 1 1 0 -1", &st, &th) != 0);
        assert(budyk_linux_parse_proc_stat("no parens here", &st, &th) != 0);

        budyk_proc_read_c r;
        uint32_t flags = 0;
        assert(budyk_linux_parse_proc_stat_ex(
            "4242 (my (odd) proc) R 1 42 42 0 -1 2097216 100 0 0 0 250 70 9 9 20 0 3 0 777 "
            "123456 1500 18446744073709551615 1 1\n", &r, &flags) == 0);
        assert(r.pid == 4242 && std::string(r.name) == "my (odd) proc" && r.state == 'R');
        assert(flags == 2097216 && (flags & 0x00200000u) != 0);
        assert(r.cpu == 320 && r.threads == 3 && r.start == 777);
        assert(r.rss_bytes == 1500ULL * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)));
        assert(budyk_linux_parse_proc_stat_ex(
            "7 (abcdefghijklmnopqrstuvwxyz0123456789) S 1 1 1 0 -1 0 0 0 0 0 1 1 0 0 20 0 1\n",
            &r, &flags) == 0);
        assert(std::string(r.name) == "abcdefghijklmnopqrstuvwxyz01234" && r.start == 0 &&
               r.rss_bytes == 0 && r.cpu == 2);
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
        // A name with a line break (prctl(PR_SET_NAME, "miner\n") is one
        // call for any process): read up to a newline, its line had no
        // ')' and the process hid from the counts and the top list.
        fx.file("110/stat", "110 (miner\n) R 1 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 9 0 1 0\n");
        budyk_linux_set_proc_root(fx.root.c_str());
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_proc_linux(&s) == 0);
        assert(s.proc.total == 9 && s.proc.running == 2 && s.proc.sleeping == 3);
        assert(s.proc.blocked == 1 && s.proc.stopped == 2 && s.proc.zombie == 1);
        assert(s.proc.threads == 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9);
    }

    // 7g2. The top processes against a fixture /proc: CPU% from the
    //      change in utime + stime over the time between reads (clock
    //      ticks, sysconf(_SC_CLK_TCK)); kernel threads left out; a pid
    //      reused (another starttime) starts from 0; an idle process is
    //      listed for its RSS; largest CPU first.
    {
        Fixture fx;
        const long tck  = sysconf(_SC_CLK_TCK);
        const long page = sysconf(_SC_PAGESIZE);
        // pid, comm, state, flags, utime, starttime, rss pages
        auto proc = [&](int pid, const char* comm, char st, unsigned flags, long utime,
                        long start, long rss) {
            char line[512];
            std::snprintf(line, sizeof(line),
                          "%d (%s) %c 1 1 1 0 -1 %u 0 0 0 0 %ld 0 0 0 20 0 1 0 %ld 1000 %ld\n",
                          pid, comm, st, flags, utime, start, rss);
            fx.file(std::to_string(pid) + "/stat", line);
        };
        proc(10, "busy one", 'R', 0, 100, 5, 10);
        proc(11, "kworker/0:1", 'I', 0x00200000u, 100, 5, 0);
        proc(12, "fat", 'S', 0, 50, 5, 100000);
        proc(13, "reused", 'S', 0, 400, 5, 20);
        budyk_linux_set_proc_root(fx.root.c_str());
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        s.timestamp_nanos = 1000000000ULL;
        assert(budyk_collect_proc_linux_ex(&s, d.get()) == 0);
        assert(s.proc.total == 4);
        assert(d->nprocs == 3);                       // no kernel thread; nothing busy yet
        for (uint32_t i = 0; i < d->nprocs; ++i) assert(d->procs[i].cpu_percent == 0.0);
        assert(d->procs[0].pid == 12);                // by RSS
        // Two seconds on: 10 used 1 s of CPU (tck ticks), 12 none, 13 is
        // a new process under the old pid.
        proc(10, "busy one", 'R', 0, 100 + tck, 5, 10);
        proc(11, "kworker/0:1", 'I', 0x00200000u, 100 + 10 * tck, 5, 0);
        proc(13, "reused", 'S', 0, 400 + tck, 9, 20);
        s.timestamp_nanos += 2000000000ULL;
        assert(budyk_collect_proc_linux_ex(&s, d.get()) == 0);
        assert(d->nprocs == 3);
        assert(d->procs[0].pid == 10 && std::string(d->procs[0].name) == "busy one");
        assert(near(d->procs[0].cpu_percent, 50.0) && d->procs[0].state == 'R');
        assert(d->procs[0].rss_bytes == 10ULL * static_cast<uint64_t>(page));
        assert(d->procs[1].pid == 12 && d->procs[1].cpu_percent == 0.0);
        assert(d->procs[2].pid == 13 && d->procs[2].cpu_percent == 0.0);   // reused: no rate
        for (uint32_t i = 0; i < d->nprocs; ++i) assert(d->procs[i].pid != 11);
        // Without d: the counts only.
        assert(budyk_collect_proc_linux_ex(&s, nullptr) == 0 && s.proc.total == 4);
    }

    // 7g3. The tracker itself: the top BUDYK_TOP_EACH by CPU and by RSS,
    //      each process once, by CPU then RSS; a process past
    //      BUDYK_MAX_TRACKED_PROCS has no rate; reads in any pid order.
    {
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        auto read = [&](uint64_t ns, int count, uint64_t cpu_step) {
            budyk_procs_begin(d.get(), ns);
            for (int i = count; i >= 1; --i) {              // descending pids
                budyk_proc_read_c r;
                std::memset(&r, 0, sizeof(r));
                r.pid       = i;
                r.start     = 1;
                r.cpu       = static_cast<uint64_t>(i) * cpu_step;   // more CPU for higher pids
                r.rss_bytes = static_cast<uint64_t>(count - i + 1) * 1000; // more RSS for lower pids
                r.state     = 'S';
                std::snprintf(r.name, sizeof(r.name), "p%d", i);
                budyk_procs_add(d.get(), &r, 100.0);
            }
            budyk_procs_end(d.get());
        };
        read(1000000000ULL, 50, 0);
        assert(d->nprocs == BUDYK_TOP_EACH);                // RSS only: none busy
        read(2000000000ULL, 50, 100);                      // pid i used i s in 1 s
        assert(d->nprocs == 2 * BUDYK_TOP_EACH);
        assert(d->procs[0].pid == 50 && near(d->procs[0].cpu_percent, 5000.0));
        assert(d->procs[BUDYK_TOP_EACH - 1].pid == 41);
        assert(d->procs[BUDYK_TOP_EACH].pid == 10 && d->procs[2 * BUDYK_TOP_EACH - 1].pid == 1);
        for (uint32_t i = 1; i < d->nprocs; ++i) {
            assert(d->procs[i - 1].cpu_percent >= d->procs[i].cpu_percent);
        }
        // Both tops at once: the union holds each once.
        budyk_procs_begin(d.get(), 3000000000ULL);
        budyk_proc_read_c big;
        std::memset(&big, 0, sizeof(big));
        big.pid = 7; big.start = 1; big.cpu = 7 * 200; big.rss_bytes = 1ULL << 40;
        budyk_procs_add(d.get(), &big, 100.0);
        budyk_procs_end(d.get());
        assert(d->nprocs == 1 && d->procs[0].pid == 7);

        // More processes than tracked: the ones read past the limit
        // (here the last five, pids 5..1, listed for their RSS) read 0.
        const int many = BUDYK_MAX_TRACKED_PROCS + 5;
        read(4000000000ULL, many, 0);
        read(5000000000ULL, many, 100);
        assert(d->nproc_track[d->proc_cur] == BUDYK_MAX_TRACKED_PROCS);
        assert(d->procs[0].pid == many && d->procs[0].cpu_percent > 0.0);
        int untracked = 0;
        for (uint32_t i = 0; i < d->nprocs; ++i) {
            if (d->procs[i].pid <= 5) {
                assert(d->procs[i].cpu_percent == 0.0);
                ++untracked;
            } else if (d->procs[i].pid <= 10) {
                assert(d->procs[i].cpu_percent > 0.0);
            }
        }
        assert(untracked == 5);
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
        // UDP over IPv6 is counted apart, in snmp6, and added: 40 more
        // datagrams in and 2 more errors in these 2 s. (IPv6's UDP read 0.)
        fx.file("net/snmp6", "Ip6InReceives 5\nUdp6InDatagrams 1000\nUdp6NoPorts 0\n"
                             "Udp6InErrors 7\nUdp6OutDatagrams 50\nUdpLite6InDatagrams 9\n");
        s.timestamp_nanos = 3'500'000'000ULL;
        assert(budyk_collect_netstat_linux(&ctx, &s) == 0);   // IPv6 appears: a jump, once
        fx.file("net/snmp6", "Ip6InReceives 5\nUdp6InDatagrams 1040\nUdp6NoPorts 0\n"
                             "Udp6InErrors 9\nUdp6OutDatagrams 50\nUdpLite6InDatagrams 9\n");
        snmp(30, 6, 54, 340);                               // and 40 over IPv4
        s.timestamp_nanos = 5'500'000'000ULL;
        assert(budyk_collect_netstat_linux(&ctx, &s) == 0);
        assert(near(s.udp.in_datagrams_per_sec, 40.0));     // (40 + 40) / 2 s
        assert(near(s.udp.in_errors_per_sec, 1.0));
        ::unlink((fx.root + "/net/snmp6").c_str());
        snmp(5, 6, 54, 300);                                // a counter went back
        s.timestamp_nanos = 6'500'000'000ULL;
        assert(budyk_collect_netstat_linux(&ctx, &s) == 0);
        assert(s.tcp.active_opens_per_sec == 0.0);

        fx.file("net/snmp", "Ip: Forwarding\nIp: 1\n");   // no Tcp/Udp lines
        assert(budyk_collect_netstat_linux(&ctx, &s) != 0);
        assert(!s.tcp.present && !s.udp.present && !s.icmp.present);
    }

    // 7h2. ICMP from the "Icmp:" pair, not "IcmpMsg:" (whose columns
    //      differ); by name, rates over the elapsed time.
    {
        Fixture fx;
        auto snmp = [&](unsigned in, unsigned out, unsigned err, unsigned unr) {
            char buf[1024];
            std::snprintf(buf, sizeof(buf),
                "Icmp: InMsgs InErrors InCsumErrors InDestUnreachs OutMsgs OutErrors\n"
                "Icmp: %u %u 0 %u %u 0\n"
                "IcmpMsg: InType3 OutType3 InMsgs\n"
                "IcmpMsg: 7 9 999999\n", in, err, unr, out);
            fx.file("net/snmp", buf);
        };
        budyk_linux_set_proc_root(fx.root.c_str());
        budyk_netstat_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        snmp(100, 50, 2, 10);
        s.timestamp_nanos = 1'000'000'000ULL;
        assert(budyk_collect_netstat_linux(&ctx, &s) == 0);   // ICMP alone is enough
        assert(s.icmp.present && !s.tcp.present && s.icmp.in_msgs_per_sec == 0.0);
        snmp(140, 70, 6, 30);
        s.timestamp_nanos = 3'000'000'000ULL;                  // 2 s later
        assert(budyk_collect_netstat_linux(&ctx, &s) == 0);
        assert(near(s.icmp.in_msgs_per_sec, 20.0) && near(s.icmp.out_msgs_per_sec, 10.0));
        assert(near(s.icmp.in_errors_per_sec, 2.0) && near(s.icmp.in_dest_unreachs_per_sec, 10.0));
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
    //     read-only filesystems are skipped (a mount read-only only in
    //     this namespace is not one), a second mount of the same
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
        // mountinfo: mount ID, parent, dev, root, mount point, the mount's
        // options, optional fields, "-", type, source, the filesystem's
        // own options.
        fx.file("self/mountinfo",
            "20 1 0:5 / /proc rw - proc proc rw\n"
            "21 1 0:6 / /sys rw - sysfs sysfs rw\n"
            // Read-only only in this mount namespace (systemd's
            // ProtectSystem=strict): the filesystem itself is rw, counted.
            "22 1 8:1 / " + fx.root + "/with\\040space ro,relatime shared:1 - ext4 /dev/sdz1 rw,errors=remount-ro\n"
            "23 1 8:3 / /dev ro - ext4 /dev/sdz3 ro\n"                 // read-only filesystem
            "24 1 8:5 / /dev rw - ext4 /dev/sdz5 ro\n"                 // the same, mounted rw
            "25 1 0:40 / /dev rw - nfs4 server:/x rw\n"               // network
            "26 1 0:41 / /dev rw - fuse.sshfs sshfs#u@h: rw\n"        // FUSE
            "27 1 7:0 / /dev rw - squashfs /dev/loop0 ro\n"           // an image
            "28 1 0:26 / /dev/shm rw,nosuid - tmpfs tmpfs rw\n"       // counted
            "29 1 0:26 / /dev/shm rw - tmpfs tmpfs rw\n"              // the same again
            "30 1 8:9 / /nonexistent-mount rw - ext4 /dev/sdz9 rw\n"); // gone
        budyk_linux_set_proc_root(fx.root.c_str());
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_fs_linux(&s) == 0);
        assert(s.fs.present);
        assert(s.fs.count == (shm ? 2u : 1u));
        // Which ones: the one read-only only in this namespace, not the
        // read-only filesystem mounted rw.
        std::unique_ptr<budyk_detail_c> det(new budyk_detail_c());
        std::memset(&s, 0, sizeof(s));
        assert(budyk_collect_fs_linux_ex(&s, det.get()) == 0);
        assert(det->nmounts == s.fs.count);
        assert(fx.root + "/with space" == det->mounts[0].mount);
        for (uint32_t i = 0; i < det->nmounts; ++i) assert(std::strcmp(det->mounts[i].mount, "/dev") != 0);
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
        // The totals are each disk's own delta summed: a disk that
        // appears is a baseline there too. (The delta of the summed
        // counters added all of sdb's 8 sectors at once.)
        assert(s.disk.read_bytes_per_sec == 2000 * 512 / 2);
        s.timestamp_nanos = 13'000'000'000ULL;
        diskstats(4000, 2000, 30, 20, 1100, false);                     // sdb gone, sda reads on
        assert(budyk_collect_disk_linux_ex(&dctx, &s, d.get()) == 0);
        assert(d->ndisks == 1 && d->ndisk_prev == 1);
        assert(s.disk.read_bytes_per_sec == 1000 * 512);               // sda's, not 0

        // The partitions of a disk whose name ends in a digit are
        // <disk>p<N> (zd0p1, a Proxmox VM's zvol partition; rbd0p1;
        // pmem0p1), and a zvol, bcache or drbd device is counted again on
        // the disks under it: each was a disk of its own in the totals.
        // Where sysfs has the device, its `partition` file decides.
        fx.file("sys/class/block/odd7/partition", "1\n");
        fx.file("sys/class/block/sdc/size", "1\n");
        auto many = [&](unsigned sectors) {
            std::string b;
            for (const char* n : {"sda", "zd0", "zd0p1", "rbd0", "rbd0p1", "pmem0", "pmem0p1",
                                  "nvme0n1", "nvme0n1p2", "bcache0", "drbd1", "odd7", "sdc"}) {
                char line[160];
                std::snprintf(line, sizeof(line), "   1 0 %s 0 0 %u 0 0 0 0 0 0 0 0\n", n, sectors);
                b += line;
            }
            fx.file("diskstats", b);
        };
        budyk_disk_ctx_c mctx;
        std::memset(&mctx, 0, sizeof(mctx));
        s.timestamp_nanos = 30'000'000'000ULL;
        many(0);
        assert(budyk_collect_disk_linux_ex(&mctx, &s, d.get()) == 0);
        s.timestamp_nanos = 31'000'000'000ULL;
        many(2048);                                // 1 MiB read on each, in 1 s
        assert(budyk_collect_disk_linux_ex(&mctx, &s, d.get()) == 0);
        std::string names;
        for (uint32_t i = 0; i < d->ndisks; ++i) names += std::string(d->disks[i].name) + " ";
        if (names != "sda rbd0 pmem0 nvme0n1 sdc ") {
            std::fprintf(stderr, "disks: %s\n", names.c_str());
            assert(false);
        }
        assert(s.disk.device_count == 5);
        assert(s.disk.read_bytes_per_sec == 5ULL << 20);

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
        assert(s.net.rx_bytes_per_sec == 0 && s.net.tx_bytes_per_sec == 200);

        // An interface that comes with 40 GB on its counters (a jail's
        // vnet back on the host, a moved netns) is a baseline in the
        // total too; one that goes (a container's veth) takes nothing
        // with it. The delta of the summed counters read 40 GB/s for the
        // first and 0 B/s for the second while eth0 moved 1 MB/s.
        auto netdev2 = [&](unsigned long long eth0_rx, const char* extra) {
            char buf[1024];
            std::snprintf(buf, sizeof(buf),
                "Inter-|   Receive  |  Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n"
                "  eth0: %llu 10 0 0 0 0 0 0 900 20 0 0 0 0 0 0\n%s", eth0_rx, extra);
            fx.file("net/dev", buf);
        };
        const char* veth = "veth9: 40000000000 1 0 0 0 0 0 0 40000000000 1 0 0 0 0 0 0\n";
        s.timestamp_nanos = 23'000'000'000ULL;
        netdev2(1'000'100, veth);               // veth9 appears; eth0 +1 MB
        assert(budyk_collect_network_linux(&nctx, &s) == 0);
        assert(s.net.rx_bytes_per_sec == 1'000'000 && s.net.tx_bytes_per_sec == 0);
        s.timestamp_nanos = 24'000'000'000ULL;
        netdev2(2'000'100, "");                 // veth9 gone; eth0 +1 MB
        assert(budyk_collect_network_linux(&nctx, &s) == 0);
        assert(s.net.rx_bytes_per_sec == 1'000'000);

        budyk_cpu_ctx_c c2;
        std::memset(&c2, 0, sizeof(c2));
        assert(budyk_collect_cpu_linux_ex(&c2, &s, nullptr) == 0);     // no detail: fine
        budyk_linux_set_proc_root(nullptr);
    }

    // 8c2. The totals of a busy host over an L1 interval of 5 minutes:
    //      60 GB written and 30 GB received are 200 MB/s and 100 MB/s
    //      (delta x 1e9 in 64 bits overflowed above 18.4 GB), and the
    //      per-device rates agree.
    {
        Fixture fx;
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        budyk_linux_set_proc_root(fx.root.c_str());
        auto files = [&](unsigned long long wsect, unsigned long long rx) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "   8       0 sda 1 0 8 0 1 0 %llu 0 0 1 0\n", wsect);
            fx.file("diskstats", buf);
            std::snprintf(buf, sizeof(buf),
                "Inter-|   Receive  |  Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n"
                "  eth0: %llu 10 0 0 0 0 0 0 1000 20 0 0 0 0 0 0\n", rx);
            fx.file("net/dev", buf);
        };
        budyk_disk_ctx_c dctx;
        budyk_net_ctx_c nctx;
        std::memset(&dctx, 0, sizeof(dctx));
        std::memset(&nctx, 0, sizeof(nctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        s.timestamp_nanos = 1'000'000'000ULL;
        files(0, 0);
        assert(budyk_collect_disk_linux_ex(&dctx, &s, d.get()) == 0);
        assert(budyk_collect_network_linux_ex(&nctx, &s, d.get()) == 0);
        s.timestamp_nanos = 301'000'000'000ULL;                        // 300 s later
        files(60000000000ULL / 512, 30000000000ULL);
        assert(budyk_collect_disk_linux_ex(&dctx, &s, d.get()) == 0);
        assert(budyk_collect_network_linux_ex(&nctx, &s, d.get()) == 0);
        assert(s.disk.write_bytes_per_sec == 200000000ULL);
        assert(d->disks[0].write_bytes_per_sec == 200000000ULL);
        assert(s.net.rx_bytes_per_sec == 100000000ULL && s.net.tx_bytes_per_sec == 0);
        assert(d->ifaces[0].rx_bytes_per_sec == 100000000ULL);
        budyk_linux_set_proc_root(nullptr);
    }

    // 8c3. A storage server and a container host: 300 disks and 1000
    //      interfaces. The totals sum every one (they followed 256: the
    //      rest moved nothing), the lists hold 256 of each (64 before),
    //      and a device that joins in the middle of the list is a
    //      baseline while those after it are still found.
    {
        Fixture fx;
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        budyk_linux_set_proc_root(fx.root.c_str());
        auto files = [&](unsigned long long step, bool joined) {
            std::string ds, nd =
                "Inter-|   Receive  |  Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n";
            char buf[256];
            for (unsigned i = 0; i < 300; ++i) {
                std::snprintf(buf, sizeof(buf), " 259 %u nvme%un1 1 0 %llu 0 1 0 %llu 0 0 1 0\n",
                              i, i, 2 * step, 4 * step);
                ds += buf;
            }
            for (unsigned i = 0; i < 1000; ++i) {
                if (joined && i == 500) {
                    nd += "  vethnew: 90000000000 1 0 0 0 0 0 0 90000000000 1 0 0 0 0 0 0\n";
                }
                std::snprintf(buf, sizeof(buf), "  veth%u: %llu 1 0 0 0 0 0 0 %llu 1 0 0 0 0 0 0\n",
                              i, 1000 * step, 10 * step);
                nd += buf;
            }
            fx.file("diskstats", ds);
            fx.file("net/dev", nd);
        };
        budyk_disk_ctx_c dctx;
        budyk_net_ctx_c nctx;
        std::memset(&dctx, 0, sizeof(dctx));
        std::memset(&nctx, 0, sizeof(nctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        for (unsigned long long t = 1; t <= 3; ++t) {
            s.timestamp_nanos = t * 1'000'000'000ULL;
            files(t, t == 3);                     // vethnew joins on the third read
            assert(budyk_collect_disk_linux_ex(&dctx, &s, d.get()) == 0);
            assert(budyk_collect_network_linux_ex(&nctx, &s, d.get()) == 0);
        }
        assert(s.disk.device_count == 300 && dctx.ndev == 300);
        assert(s.disk.read_bytes_per_sec == 300ULL * 2 * 512);         // every disk, a second
        assert(s.disk.write_bytes_per_sec == 300ULL * 4 * 512);
        assert(s.net.interface_count == 1001 && nctx.ndev == 1001);
        assert(s.net.rx_bytes_per_sec == 1000ULL * 1000);              // vethnew: a baseline
        assert(s.net.tx_bytes_per_sec == 1000ULL * 10);
        assert(d->ndisks == BUDYK_MAX_ENTITIES && d->ndisks == 256);
        assert(d->nifaces == 256);
        assert(d->disks[255].read_bytes_per_sec == 2 * 512);
        budyk_linux_set_proc_root(nullptr);
    }

    // 8c5. The network totals are the devices' traffic: a container's
    //      packet goes through its veth, docker0 and eth0, and each
    //      counted it. Only an interface with /sys/class/net/<if>/device
    //      counts, and not one enslaved to another device, whose
    //      counters have its traffic (Azure's VF enP7s1 under eth0); a
    //      bond's ports eth1 and eth2 do count. With no device (budyk in
    //      a container: its eth0 is a veth), every one does. The list
    //      has them all.
    {
        Fixture fx;
        budyk_linux_set_proc_root(fx.root.c_str());
        const std::string net = fx.root + "/sys/class/net/";
        for (const char* dev : {"eth0", "enP7s1", "eth1", "eth2"}) {
            fx.file(std::string("sys/class/net/") + dev + "/device/vendor", "0x8086\n");
        }
        assert(::symlink("../eth0", (net + "enP7s1/master").c_str()) == 0);
        assert(::symlink("../../../virtual/net/bond0", (net + "eth1/master").c_str()) == 0);
        assert(::symlink("../bond0", (net + "eth2/master").c_str()) == 0);
        const char* const names[] = {"eth0", "enP7s1", "eth1", "eth2", "bond0",
                                     "docker0", "veth1a2b", "br0"};
        auto netdev = [&](unsigned long long n) {
            std::string nd =
                "Inter-|   Receive  |  Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n";
            char buf[160];
            for (const char* name : names) {
                std::snprintf(buf, sizeof(buf), "  %s: %llu 1 0 0 0 0 0 0 %llu 1 0 0 0 0 0 0\n",
                              name, 1000 * n, 100 * n);
                nd += buf;
            }
            fx.file("net/dev", nd);
        };
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        budyk_net_ctx_c nctx;
        std::memset(&nctx, 0, sizeof(nctx));
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        for (unsigned long long t = 1; t <= 2; ++t) {
            s.timestamp_nanos = t * 1'000'000'000ULL;
            netdev(t);
            assert(budyk_collect_network_linux_ex(&nctx, &s, d.get()) == 0);
        }
        // eth0, eth1 and eth2
        assert(s.net.rx_bytes_per_sec == 3 * 1000 && s.net.tx_bytes_per_sec == 3 * 100);
        assert(s.net.interface_count == 8 && d->nifaces == 8);
        assert(std::strcmp(d->ifaces[5].name, "docker0") == 0);
        assert(d->ifaces[5].rx_bytes_per_sec == 1000);                           // listed
        const std::string rm = "rm -rf " + net + "*/device";
        assert(std::system(rm.c_str()) == 0);
        s.timestamp_nanos = 3'000'000'000ULL;
        netdev(3);
        assert(budyk_collect_network_linux_ex(&nctx, &s, d.get()) == 0);
        assert(s.net.rx_bytes_per_sec == 8 * 1000 && s.net.tx_bytes_per_sec == 8 * 100);
        budyk_linux_set_proc_root(nullptr);
    }

    // 8c4. budyk_counted_add() finds each device where the last one was
    //      found: in the same order, a device is the first one it looks
    //      at; after one that joined or left, the next is found again.
    {
        budyk_counted_c prev[3] = {{"a", 1, 1}, {"b", 1, 1}, {"c", 1, 1}};
        budyk_counted_c next[4];
        uint32_t nnext = 0, pos = 0;
        uint64_t ma = 0, mb = 0;
        budyk_counted_add(prev, 3, &pos, next, &nnext, "a", 2, 2, &ma, &mb);
        assert(pos == 1 && ma == 1);
        budyk_counted_add(prev, 3, &pos, next, &nnext, "x", 9, 9, &ma, &mb);   // joined
        assert(pos == 1 && ma == 1 && nnext == 2);
        budyk_counted_add(prev, 3, &pos, next, &nnext, "c", 5, 5, &ma, &mb);   // b left
        assert(pos == 3 && ma == 5 && mb == 5);
        budyk_counted_add(prev, 3, &pos, next, &nnext, "b", 3, 3, &ma, &mb);   // round the list
        assert(pos == 2 && ma == 7 && nnext == 4);
        assert(std::strcmp(next[1].name, "x") == 0 && next[1].a == 9);
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
