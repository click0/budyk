// SPDX-License-Identifier: BSD-3-Clause
#include "core/metric_set.h"
#include "core/sample.h"
#include "web/json.h"

#include <cassert>
#include <memory>
#include <cmath>
#include <cstdio>
#include <string>

using namespace budyk;

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // 1. Empty samples list — count=0, samples=[].
    {
        std::string doc = samples_to_json(nullptr, 0);
        assert(contains(doc, "\"count\":0"));
        assert(contains(doc, "\"samples\":[]"));
    }

    // 2. Single populated sample — every section appears with the
    //    expected key:value shape.
    {
        Sample s{};
        s.timestamp_nanos          = 1700000000000000000ULL;
        s.level                    = Level::L3;
        s.cpu.total_percent        = 42.5;
        s.cpu.count                = 4;
        s.mem.total                = 16ULL << 30;
        s.mem.available            = 4ULL << 30;
        s.mem.available_percent    = 25.0;
        s.swap.total               = 1ULL << 30;
        s.swap.used                = 256ULL << 20;
        s.swap.used_percent        = 25.0;
        s.load.avg_1m              = 0.75;
        s.load.avg_5m              = 1.10;
        s.load.avg_15m             = 1.50;
        s.disk.read_bytes_per_sec  = 50ULL * 1024 * 1024;
        s.disk.write_bytes_per_sec = 30ULL * 1024 * 1024;
        s.disk.device_count        = 2;
        s.net.rx_bytes_per_sec     = 5ULL * 1024 * 1024;
        s.net.tx_bytes_per_sec     = 1ULL * 1024 * 1024;
        s.net.interface_count      = 3;
        s.proc.total               = 235;
        s.proc.running             = 4;
        s.entropy.available_bits   = 512;
        s.entropy.present          = true;
        s.self_.rss_bytes          = 16ULL * 1024 * 1024;
        s.self_.peak_rss_bytes     = 32ULL * 1024 * 1024;
        s.self_.cpu_user_seconds   = 2.5;
        s.self_.cpu_system_seconds = 0.5;
        s.thermal.max_celsius      = 67.5;
        s.thermal.sensor_count     = 4;
        s.thermal.present          = true;
        s.cpu.user_percent         = 30.0;
        s.cpu.iowait_percent       = 2.5;
        s.cpu.idle_percent         = 57.5;
        s.proc.zombie              = 1;
        s.proc.threads             = 980;
        s.tcp.established          = 42;
        s.tcp.retransmits_per_sec  = 1.5;
        s.tcp.present              = true;
        s.udp.no_port_per_sec      = 0.25;
        s.udp.present              = true;
        s.icmp.in_msgs_per_sec     = 12.5;
        s.icmp.in_dest_unreachs_per_sec = 0.5;
        s.icmp.present             = true;
        s.fs.count                 = 3;
        s.fs.max_used_percent      = 91.5;
        s.fs.present               = true;
        s.clock.offset_ms          = -0.75;
        s.clock.synced             = true;
        s.clock.present            = true;
        s.uptime_seconds           = 7200.5;

        std::string j = sample_to_json(s);
        assert(j.front() == '{' && j.back() == '}');
        assert(contains(j, "\"ts\":1700000000000000000"));
        assert(contains(j, "\"level\":3"));
        assert(contains(j, "\"cpu\":{\"total_percent\":42.5,\"count\":4,\"user_percent\":30,"));
        assert(contains(j, "\"mem\":{"));
        assert(contains(j, "\"available_percent\":25"));
        assert(contains(j, "\"swap\":{"));
        assert(contains(j, "\"load\":{\"avg_1m\":0.75"));
        assert(contains(j, "\"disk\":{"));
        assert(contains(j, "\"device_count\":2"));
        assert(contains(j, "\"iowait_percent\":2.5,"));
        assert(contains(j, "\"idle_percent\":57.5}"));
        assert(contains(j, "\"zombie\":1,\"threads\":980}"));
        assert(contains(j, "\"tcp\":{\"established\":42,\"time_wait\":0,"));
        assert(contains(j, "\"retransmits_per_sec\":1.5,"));
        assert(contains(j, "\"udp\":{") && contains(j, "\"no_port_per_sec\":0.25,"));
        assert(contains(j, "\"icmp\":{\"in_msgs_per_sec\":12.5,\"out_msgs_per_sec\":0,"
                           "\"in_errors_per_sec\":0,\"in_dest_unreachs_per_sec\":0.5,"
                           "\"present\":true},"));
        assert(contains(j, "\"fs\":{\"count\":3,\"max_used_percent\":91.5,"));
        assert(contains(j, "\"clock\":{\"offset_ms\":-0.75,\"max_error_ms\":0,"
                           "\"synced\":true,\"present\":true}"));
        assert(contains(j, "\"net\":{"));
        assert(contains(j, "\"interface_count\":3"));
        assert(contains(j, "\"proc\":{"));
        assert(contains(j, "\"total\":235"));
        assert(contains(j, "\"running\":4"));
        assert(contains(j, "\"entropy\":{"));
        assert(contains(j, "\"available_bits\":512"));
        assert(contains(j, "\"present\":true"));
        assert(contains(j, "\"self\":{"));
        assert(contains(j, "\"cpu_user_seconds\":2.5"));
        assert(contains(j, "\"cpu_system_seconds\":0.5"));
        assert(contains(j, "\"thermal\":{"));
        assert(contains(j, "\"max_celsius\":67.5"));
        assert(contains(j, "\"sensor_count\":4"));
        assert(contains(j, "\"uptime_seconds\":7200.5"));
    }

    // 3. Three samples — count and the comma separator land correctly.
    {
        Sample arr[3]{};
        for (int i = 0; i < 3; ++i) {
            arr[i].timestamp_nanos    = static_cast<uint64_t>(1000 + i);
            arr[i].level              = Level::L2;
            arr[i].cpu.total_percent  = 10.0 * (i + 1);
            arr[i].cpu.count          = 1;
        }
        std::string doc = samples_to_json(arr, 3);
        assert(contains(doc, "\"count\":3"));
        assert(contains(doc, "\"ts\":1000"));
        assert(contains(doc, "\"ts\":1001"));
        assert(contains(doc, "\"ts\":1002"));
        assert(contains(doc, "\"total_percent\":10"));
        assert(contains(doc, "\"total_percent\":20"));
        assert(contains(doc, "\"total_percent\":30"));
        // 3 sample objects are joined by exactly 2 commas at the top
        // level — those between '},{', not the inner ones.
        size_t boundary = 0, pos = 0;
        while ((pos = doc.find("},{", pos)) != std::string::npos) {
            ++boundary;
            ++pos;
        }
        assert(boundary == 2);
    }

    // 4. NaN / Inf collapse to 0 — JSON has no spec for them, the
    //    daemon never produces them in well-formed paths, but we
    //    guarantee we don't emit invalid JSON if they sneak in.
    {
        Sample s{};
        s.level                = Level::L1;
        s.cpu.total_percent    = std::nan("");
        s.uptime_seconds       = 1.0 / 0.0;          // +Inf
        std::string j = sample_to_json(s);
        assert(!contains(j, "nan"));
        assert(!contains(j, "inf"));
        assert(contains(j, "\"total_percent\":0"));
        assert(contains(j, "\"uptime_seconds\":0"));
    }

    // 4b. A double from a million up is written whole, or with 15 digits
    //     when it has a fraction: %.6g wrote an uptime of 17280042 s as
    //     1.728e+07, 42 s short, and past 115 days the minute the clients
    //     showed could be off by one. Small values keep their 6 digits.
    {
        Sample s{};
        s.level          = Level::L1;
        s.uptime_seconds = 17280042.0;
        assert(contains(sample_to_json(s), "\"uptime_seconds\":17280042}"));
        s.uptime_seconds = 100000059.5;
        assert(contains(sample_to_json(s), "\"uptime_seconds\":100000059.5}"));
        s.uptime_seconds = 7200.5;
        assert(contains(sample_to_json(s), "\"uptime_seconds\":7200.5}"));
        s.uptime_seconds = 1.0 / 3.0;
        assert(contains(sample_to_json(s), "\"uptime_seconds\":0.333333}"));
    }

    // 5. json_unescape: every escape JSON.stringify can emit, \u in
    //    all three UTF-8 widths, and the lenient cases (unknown escape,
    //    trailing backslash, short \u, bad hex digit).
    {
        assert(json_unescape("plain") == "plain");
        assert(json_unescape("a\\\"b\\\\c\\/d") == "a\"b\\c/d");
        assert(json_unescape("\\b\\f\\n\\r\\t") == "\b\f\n\r\t");
        assert(json_unescape("\\u0041") == "A");
        assert(json_unescape("\\u00e9") == "\xC3\xA9");          // é
        assert(json_unescape("\\u0443") == "\xD1\x83");          // у
        assert(json_unescape("\\u20ac") == "\xE2\x82\xAC");      // €
        // A surrogate pair is one character past U+FFFF, in 4 bytes (it
        // was 6, CESU-8: a password with an emoji, escaped, never
        // matched); a lone surrogate is U+FFFD.
        assert(json_unescape("p\\ud83d\\ude00w") == "p\xF0\x9F\x98\x80w");   // 😀
        assert(json_unescape("\\uD83D\\uDE00") == "\xF0\x9F\x98\x80");
        assert(json_unescape("\\ud83dx") == "\xEF\xBF\xBDx");
        assert(json_unescape("\\ude00") == "\xEF\xBF\xBD");
        assert(json_unescape("\\ud83d\\u0041") == "\xEF\xBF\xBD" "A");
        assert(json_unescape("\\q") == "q");
        assert(json_unescape("end\\") == "end\\");
        assert(json_unescape("\\u12") == "\\u12");
        assert(json_unescape("\\u12G4") == "\xEF\xBF\xBD");      // U+FFFD
    }

    // 5b. json_escape, the one implementation the alert payloads, the
    //     LLM client and the TUI share: the named escapes, other control
    //     characters as \u00xx, UTF-8 untouched, and a round trip.
    {
        assert(json_escape("plain") == "plain");
        assert(json_escape("a\"b\\c") == "a\\\"b\\\\c");
        assert(json_escape("\b\f\n\r\t") == "\\b\\f\\n\\r\\t");
        assert(json_escape(std::string("\x01\x1f", 2)) == "\\u0001\\u001f");
        assert(json_escape("\xD1\x83") == "\xD1\x83");                 // у, as is
        assert(json_escape("/") == "/");
        const std::string all = std::string("q\"b\\\x01\n\xE2\x82\xAC", 9);
        assert(json_unescape(json_escape(all)) == all);
        // Not UTF-8 (a process name or a mount point can be any bytes):
        // each byte that starts no well-formed sequence is \ufffd, so the
        // JSON, and a WebSocket text frame of it, stays valid UTF-8.
        assert(json_escape("a\xFF" "b") == "a\\ufffdb");
        assert(json_escape("\xE2\x82") == "\\ufffd\\ufffd");             // cut short
        assert(json_escape("\xC0\xAF") == "\\ufffd\\ufffd");             // overlong '/'
        assert(json_escape("\xE0\x80\xAF") == "\\ufffd\\ufffd\\ufffd");   // overlong, 3 bytes
        assert(json_escape("\xE2\x82" "A") == "\\ufffd\\ufffdA");         // third byte not a continuation
        assert(json_escape("\xED\xA0\x80") == "\\ufffd\\ufffd\\ufffd");   // a surrogate
        assert(json_escape("\xF4\x90\x80\x80") == "\\ufffd\\ufffd\\ufffd\\ufffd");  // > U+10FFFF
        assert(json_escape("\xF0\x9F\x98\x80\xEF\xBF\xBD") == "\xF0\x9F\x98\x80\xEF\xBF\xBD");
        assert(json_escape("\xE0\xA0\x80\xF0\x90\x80\x80") == "\xE0\xA0\x80\xF0\x90\x80\x80");
        assert(json_escape("\x80") == "\\ufffd");
        assert(json_unescape(json_escape("x\xFE")) == "x\xEF\xBF\xBD");
    }

    // 6. json_get_string: the login body as the SPA and the TUI send
    //    it, with escapes in the password, whitespace around the colon,
    //    a quote inside the value, and the failure cases.
    {
        std::string v;
        assert(json_get_string("{\"password\":\"hunter2\"}", "password", &v));
        assert(v == "hunter2");
        // A password containing " and \ — JSON.stringify output.
        assert(json_get_string("{\"password\":\"pa\\\"ss\\\\w\"}", "password", &v));
        assert(v == "pa\"ss\\w");
        assert(json_get_string("{ \"password\" :\t\"x y\" , \"o\": 1 }", "password", &v));
        assert(v == "x y");
        assert(json_get_string("{\"password\":\"\\u0441\\u043b\\u043e\\u0432\\u043e\"}", "password", &v));
        assert(v == "слово");
        assert(json_get_string("{\"password\":\"\"}", "password", &v) && v.empty());

        assert(!json_get_string("{\"user\":\"x\"}", "password", &v));
        assert(!json_get_string("{\"password\":123}", "password", &v));
        assert(!json_get_string("{\"password\":\"unterminated", "password", &v));
        assert(!json_get_string("{\"password\":\"esc\\\"", "password", &v));
        assert(!json_get_string("{}", nullptr, &v));
        assert(!json_get_string("{}", "password", nullptr));
    }

    // 7. "folded":true marks a record folded from finer samples; a live
    //    sample (and every unfolded record) has no such key.
    {
        Sample s{};
        s.timestamp_nanos = 5;
        s.level = Level::L1;
        assert(sample_to_json(s).find("folded") == std::string::npos);
        s.folded = true;
        const std::string j = sample_to_json(s);
        assert(j.find("\"level\":1,\"folded\":true,\"groups\":[],\"cpu\":{") != std::string::npos);
    }

    // 8. detail_to_json: cores, disks, interfaces, filesystems, names
    //    escaped; samples_to_json adds it as "detail" only when given and
    //    not empty.
    {
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        d->ncores = 1;
        d->cores[0].user_percent = 12.5;
        d->cores[0].id = 2;                       // cpu2: the one before it offline
        d->ndisks = 1;
        std::snprintf(d->disks[0].name, sizeof(d->disks[0].name), "sda");
        d->disks[0].busy_percent = 40.0;
        d->nifaces = 1;
        std::snprintf(d->ifaces[0].name, sizeof(d->ifaces[0].name), "eth0");
        d->ifaces[0].up = 1;
        d->nmounts = 1;
        std::snprintf(d->mounts[0].mount, sizeof(d->mounts[0].mount), "/mnt/a \"b\"");
        std::snprintf(d->mounts[0].type,  sizeof(d->mounts[0].type),  "ext4");
        d->mounts[0].total_bytes = 1000;
        const std::string j = detail_to_json(*d);
        assert(j.front() == '{' && j.back() == '}');
        assert(contains(j, "\"cores\":[{\"id\":2,\"total_percent\":0,\"user_percent\":12.5,"));
        assert(contains(j, "\"disks\":[{\"name\":\"sda\","));
        assert(contains(j, "\"busy_percent\":40}"));
        assert(contains(j, "\"interfaces\":[{\"name\":\"eth0\",") && contains(j, "\"up\":true}"));
        assert(contains(j, "\"mount\":\"/mnt/a \\\"b\\\"\",\"type\":\"ext4\","));
        assert(contains(j, "\"total_bytes\":1000,"));

        d->ncores = d->ndisks = d->nifaces = d->nmounts = 0;
        assert(detail_to_json(*d) ==
               "{\"collected\":[],\"cores\":[],\"disks\":[],\"interfaces\":[],\"filesystems\":[],"
               "\"processes\":[]}");
        d->collected = kDetailAll;
        assert(contains(detail_to_json(*d), "{\"collected\":[\"cpu.cores\",\"disk.devices\","
                                            "\"net.interfaces\",\"fs.mounts\",\"proc.top\"],\"cores\":[]"));
        // The processes: names escaped (not UTF-8 included), the state a
        // letter.
        d->nprocs = 2;
        d->procs[0] = budyk_process_c{ 77, "bad\xFF\"name", 'R', 3, 12.5, 4096 };
        d->procs[1] = budyk_process_c{ 1, "init", '\0', 1, 0.0, 0 };
        assert(contains(detail_to_json(*d),
                        "\"processes\":[{\"pid\":77,\"name\":\"bad\\ufffd\\\"name\",\"state\":\"R\","
                        "\"threads\":3,\"cpu_percent\":12.5,\"rss_bytes\":4096},"
                        "{\"pid\":1,\"name\":\"init\",\"state\":\"\",\"threads\":1,"
                        "\"cpu_percent\":0,\"rss_bytes\":0}]}"));
        d->nprocs = 0;
        d->collected = kDetailDisks | kDetailMounts;
        assert(contains(detail_to_json(*d), "{\"collected\":[\"disk.devices\",\"fs.mounts\"],\"cores\""));
        d->baseline = kDetailCores | kDetailInterfaces;
        assert(contains(detail_to_json(*d), "\"fs.mounts\"],\"baseline\":[\"cpu.cores\","
                                            "\"net.interfaces\"],\"cores\":["));
        d->baseline = 0;

        Sample x{};
        x.level = Level::L3;
        const std::string with = samples_to_json(&x, 1, &j);
        assert(contains(with, "}],\"detail\":{\"collected\":[],\"cores\":[") && with.back() == '\n');
        const std::string empty;
        assert(!contains(samples_to_json(&x, 1, &empty), "detail"));
        assert(!contains(samples_to_json(&x, 1), "detail"));
    }

    // 9. "groups" names the groups a sample has, in bit order, file_watch
    //    and the v8 ones included.
    {
        Sample s{};
        s.groups = kGroupsMinimal | kGroupFileWatch | kGroupClock;
        assert(contains(sample_to_json(s),
                        "\"groups\":[\"cpu\",\"mem\",\"swap\",\"load\",\"file_watch\","
                        "\"uptime\",\"clock\"],\"cpu\":{"));
        assert(!contains(sample_to_json(s), "baseline"));
        s.baseline = kGroupDisk | kGroupTcp;
        assert(contains(sample_to_json(s), "\"clock\"],\"baseline\":[\"disk\",\"tcp\"],\"cpu\":{"));
        s.baseline = 0;
        s.groups = kGroupsAll;
        const std::string all = sample_to_json(s);
        for (const char* g : { "\"disk\"", "\"net\"", "\"proc\"", "\"entropy\"", "\"self\"",
                               "\"thermal\"", "\"tcp\"", "\"udp\"", "\"fs\"" }) {
            assert(contains(all.substr(0, all.find("\"cpu\":{")), g));
        }
    }

    // 10. alerts_to_json: oldest first, the strings escaped.
    {
        const AlertEvent a[] = {
            { 5, "critical", "disk \"full\"", "line1\nline2" },
            { 6, "info", "x", "" },
        };
        const std::string j = alerts_to_json(a, 2);
        assert(j == "{\"count\":2,\"alerts\":[{\"ts\":5,\"severity\":\"critical\","
                    "\"rule\":\"disk \\\"full\\\"\",\"message\":\"line1\\nline2\"},"
                    "{\"ts\":6,\"severity\":\"info\",\"rule\":\"x\",\"message\":\"\"}]}\n");
        assert(alerts_to_json(nullptr, 0) == "{\"count\":0,\"alerts\":[]}\n");
    }

    std::printf("test_json: PASS\n");
    return 0;
}
