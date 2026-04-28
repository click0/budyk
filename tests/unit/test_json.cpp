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

    std::printf("test_json: PASS\n");
    return 0;
}
