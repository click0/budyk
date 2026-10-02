// SPDX-License-Identifier: BSD-3-Clause
#include "core/sample.h"
#include "web/json.h"

#include <cassert>
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
        s.uptime_seconds           = 7200.5;

        std::string j = sample_to_json(s);
        assert(j.front() == '{' && j.back() == '}');
        assert(contains(j, "\"ts\":1700000000000000000"));
        assert(contains(j, "\"level\":3"));
        assert(contains(j, "\"cpu\":{\"total_percent\":42.5,\"count\":4}"));
        assert(contains(j, "\"mem\":{"));
        assert(contains(j, "\"available_percent\":25"));
        assert(contains(j, "\"swap\":{"));
        assert(contains(j, "\"load\":{\"avg_1m\":0.75"));
        assert(contains(j, "\"disk\":{"));
        assert(contains(j, "\"device_count\":2"));
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
        assert(json_unescape("\\q") == "q");
        assert(json_unescape("end\\") == "end\\");
        assert(json_unescape("\\u12") == "\\u12");
        assert(json_unescape("\\u12G4") == "\xEF\xBF\xBD");      // U+FFFD
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

    std::printf("test_json: PASS\n");
    return 0;
}
