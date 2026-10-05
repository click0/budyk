// SPDX-License-Identifier: BSD-3-Clause
// Spec §5 item 13: 100 watch() rules are evaluated per tick within 1 ms.
//
// Usage: test_rule_perf <budget_us>
//   budget_us > 0  fail if the median tick takes longer
//   budget_us = 0  report only (sanitizer and coverage builds, whose
//                  instrumentation makes the timing meaningless)
#include "core/sample.h"
#include "rules/lua_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace budyk;

namespace {

Sample mk(int i) {
    Sample s{};
    s.timestamp_nanos         = 1;
    s.level                   = Level::L3;
    s.cpu.total_percent       = 20.0 + (i % 50);
    s.cpu.count               = 4;
    s.mem.total               = 16ULL << 30;
    s.mem.available           = 4ULL  << 30;
    s.mem.available_percent   = 25.0;
    s.swap.used_percent       = 1.0;
    s.load.avg_1m             = 0.5 + (i % 7) * 0.1;
    s.load.avg_5m             = 0.4;
    s.load.avg_15m            = 0.3;
    s.disk.read_bytes_per_sec = 1000u * static_cast<unsigned>(i % 100);
    s.net.rx_bytes_per_sec    = 5000;
    s.uptime_seconds          = 1234.5 + i;
    return s;
}

// 100 rules shaped like the ones in rules/examples.lua: plain and
// computed thresholds over several metric groups, sustain counters,
// cooldowns. A quarter fire now and then (cpu crosses 60 on part of
// the generated ticks) through a Lua-function action, so the firing
// path is timed too without printing anything.
std::string make_rules() {
    std::string out = "fired = 0\n";
    char buf[512];
    for (int i = 0; i < 100; ++i) {
        const char* when = nullptr;
        switch (i % 4) {
        case 0: when = "cpu.total_percent > 60"; break;
        case 1: when = "mem.available_percent < 5 and swap.used_percent > 50"; break;
        case 2: when = "load.avg_1m > cpu.count * 2"; break;
        default: when = "disk.read_bytes_per_sec + net.rx_bytes_per_sec > 1e12"; break;
        }
        std::snprintf(buf, sizeof buf,
            "watch('r%03d', {\n"
            "    when      = function() return %s end,\n"
            "    action    = function() fired = fired + 1 end,\n"
            "    for_ticks = %d,\n"
            "    cooldown  = 10,\n"
            "})\n",
            i, when, 1 + i % 3);
        out += buf;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const long budget_us = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 1000;

    // Explicit checks rather than assert(): the timed calls must run
    // even in a build with NDEBUG.
    LuaEngine e;
    if (e.init(false) != 0 || e.load_string(make_rules().c_str()) != 0 ||
        e.rule_count() != 100) {
        std::fprintf(stderr, "FAIL: rules did not load: %s\n", e.last_error().c_str());
        return 1;
    }

    std::vector<Sample> samples;
    for (int i = 0; i < 64; ++i) samples.push_back(mk(i));

    // Warm up the Lua VM and the allocator.
    for (int i = 0; i < 200; ++i) e.eval_tick(samples[static_cast<size_t>(i) % samples.size()]);

    constexpr int kTicks = 2000;
    std::vector<double> us;
    us.reserve(kTicks);
    long fired = 0;
    for (int i = 0; i < kTicks; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const int  n  = e.eval_tick(samples[static_cast<size_t>(i) % samples.size()]);
        const auto t1 = std::chrono::steady_clock::now();
        if (n < 0) {
            std::fprintf(stderr, "FAIL: eval_tick returned %d\n", n);
            return 1;
        }
        fired += n;
        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    e.shutdown();

    std::sort(us.begin(), us.end());
    const double median = us[us.size() / 2];
    const double p99    = us[us.size() * 99 / 100];
    std::printf("100 rules: median %.1f us, p99 %.1f us, max %.1f us per tick "
                "(%d ticks, %ld fires, budget %ld us)\n",
                median, p99, us.back(), kTicks, fired, budget_us);

    // Some rules must have fired, or the firing path wasn't timed.
    if (fired == 0) {
        std::fprintf(stderr, "FAIL: no rule fired\n");
        return 1;
    }
    // The median, not the max: a single tick can be delayed by the
    // scheduler on a busy CI runner without the engine being slow.
    if (budget_us > 0 && median > static_cast<double>(budget_us)) {
        std::fprintf(stderr, "FAIL: median %.1f us exceeds the %ld us budget\n",
                     median, budget_us);
        return 1;
    }
    return 0;
}
