// SPDX-License-Identifier: BSD-3-Clause
#include "ai/suggest.h"

#include "ai/baseline.h"
#include "core/sample.h"
#include "rules/lua_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace budyk {

namespace {

// How many ticks in a row a condition must hold before the rule fires
// (its cooldown is as many, the engine's default): spec §3.6.2's examples,
// 5 for a load, 3 for memory running out. Without it a rule fired on one
// tick and again every other tick while the condition held.
constexpr int kSustainTicks       = 5;
constexpr int kSustainTicksMemory = 3;

// One watch() call. `when_expr` is Lua up to the `end` that closes its
// function: a `--` comment in it took that `end` with it, and the file
// did not load.
std::string fmt_rule(const char* name,
                     const char* rationale,
                     const char* severity,
                     const std::string& when_expr,
                     int for_ticks) {
    std::ostringstream os;
    os << "-- " << rationale << '\n'
       << "watch(\"" << name << "\", {\n"
       << "  when      = function() return " << when_expr << " end,\n"
       << "  for_ticks = " << for_ticks << ",\n"
       << "  severity  = \"" << severity << "\",\n"
       << "  action    = alert,\n"
       << "})\n\n";
    return os.str();
}

std::string fmt_num(double v, int prec = 1) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

// No rule, and why: the template's threshold is inside what the host
// does every day, so the rule would hold all the time (swap at 96-98 %
// got `swap.used_percent > 90`, a CPU at 99 % most of the day `> 95`).
std::string fmt_no_rule(const char* name, const std::string& why) {
    return std::string("-- ") + name + ": no rule: " + why + "\n\n";
}

} // namespace

std::string suggest_rule_cpu_total_percent(const MetricBaseline& b) {
    if (b.n == 0) return {};

    // Spec §3.7.1: p99 at 45% → 85%. Threshold = max(85, ceil(p99 + 15)),
    // capped at 95 (the floor was 60: 45% gave 60, an alert at a load the
    // host had every day).
    double t = std::ceil(b.p99 + 15.0);
    if (t < 85.0) t = 85.0;
    if (t > 95.0) t = 95.0;

    std::ostringstream rat;
    rat << "Tier A: cpu.total_percent p99=" << fmt_num(b.p99)
        << "%, max=" << fmt_num(b.max) << "% over " << b.n << " samples";
    if (t <= b.p99) {
        return fmt_no_rule("high_cpu", "p99 is " + fmt_num(b.p99) + "%, at or above the " +
                           fmt_num(t, 0) + "% cap: the host runs at that load (" + rat.str() + ")");
    }
    std::ostringstream cond;
    cond << "cpu.total_percent > " << fmt_num(t, 0);
    return fmt_rule("high_cpu", rat.str().c_str(), "warning", cond.str(), kSustainTicks);
}

std::string suggest_rule_mem_available_percent(const MetricBaseline& b) {
    if (b.n == 0) return {};

    double t;
    const char* severity;
    if (b.min < 10.0) {
        t = std::floor(b.min - 2.0);
        if (t < 5.0) t = 5.0;
        severity = "critical";
    } else {
        t = 10.0;
        severity = "warning";
    }

    std::ostringstream rat;
    rat << "Tier A: mem.available_percent min=" << fmt_num(b.min)
        << "%, mean=" << fmt_num(b.mean) << "% over " << b.n << " samples";
    if (t >= b.min) {
        return fmt_no_rule("memory_low", "the minimum is " + fmt_num(b.min) + "%, at or under the " +
                           fmt_num(t, 0) + "% floor: the host runs that low (" + rat.str() + ")");
    }
    std::ostringstream cond;
    cond << "mem.available_percent < " << fmt_num(t, 0);
    return fmt_rule("memory_low", rat.str().c_str(), severity, cond.str(), kSustainTicksMemory);
}

std::string suggest_rule_swap_used_percent(const MetricBaseline& b) {
    if (b.n == 0)     return {};
    if (b.max < 1.0)  return {};     // swap never used — no rule warranted

    double t = std::ceil(b.p99 * 2.0);
    if (t < 50.0) t = 50.0;
    if (t > 90.0) t = 90.0;
    if (t <= b.p99) {
        return fmt_no_rule("swap_pressure", "p99 is " + fmt_num(b.p99) + "%, at or above the " +
                           fmt_num(t, 0) + "% cap: the host runs on swap (max " +
                           fmt_num(b.max) + "% over " + std::to_string(b.n) + " samples)");
    }

    std::ostringstream rat;
    rat << "Tier A: swap.used_percent p99=" << fmt_num(b.p99)
        << "%, max=" << fmt_num(b.max) << "% over " << b.n << " samples";
    std::ostringstream cond;
    cond << "swap.used_percent > " << fmt_num(t, 0);
    return fmt_rule("swap_pressure", rat.str().c_str(), "warning", cond.str(), kSustainTicks);
}

std::string suggest_rule_load_1m(const MetricBaseline& b, double cpu_count) {
    if (b.n == 0) return {};

    double scaled = std::ceil(b.p99 * 1.5);
    double cap    = cpu_count > 0 ? cpu_count * 1.5 : 0.0;
    double t      = scaled > cap ? scaled : cap;
    if (t < 2.0) t = 2.0;

    std::ostringstream rat;
    rat << "Tier A: load.avg_1m p99=" << fmt_num(b.p99, 2)
        << ", max=" << fmt_num(b.max, 2) << " over " << b.n << " samples";
    std::ostringstream cond;
    cond << "load.avg_1m > " << fmt_num(t, 1);
    return fmt_rule("load_high", rat.str().c_str(), "warning", cond.str(), kSustainTicks);
}

// ---------------------------------------------------------------------------
// Throughput (bytes/sec) suggestions — shared shape for disk + net.
// ---------------------------------------------------------------------------
namespace {

std::string fmt_bps(double bps) {
    char buf[64];
    if (bps >= 1024.0 * 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f GiB/s", bps / (1024.0 * 1024.0 * 1024.0));
    } else if (bps >= 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f MiB/s", bps / (1024.0 * 1024.0));
    } else if (bps >= 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f KiB/s", bps / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f B/s", bps);
    }
    return buf;
}

// Threshold for throughput: ceil(p99 * 1.5), floored at min_floor_mb MiB/s.
// Metric only produces a rule if max ≥ min_activity_mb MiB/s — otherwise
// the metric is effectively idle and a rule would false-positive on noise.
std::string suggest_throughput_rule(const MetricBaseline& b,
                                    const char* rule_name,
                                    const char* metric_expr,
                                    double min_activity_mb,
                                    double min_floor_mb) {
    if (b.n == 0)                          return {};
    const double mib = 1024.0 * 1024.0;
    if (b.max < min_activity_mb * mib)     return {};   // quiet — no rule

    double t    = std::ceil(b.p99 * 1.5);
    double floor_bytes = min_floor_mb * mib;
    if (t < floor_bytes) t = floor_bytes;

    // The threshold in words goes in the rationale: as a comment after
    // the condition it commented out the function's `end`.
    std::ostringstream rat;
    rat << "Tier A: " << metric_expr
        << " p99=" << fmt_bps(b.p99)
        << ", max=" << fmt_bps(b.max)
        << " over " << b.n << " samples; threshold " << fmt_bps(t);
    std::ostringstream cond;
    cond << metric_expr << " > " << fmt_num(t, 0);
    return fmt_rule(rule_name, rat.str().c_str(), "warning", cond.str(), kSustainTicks);
}

} // namespace

std::string suggest_rule_disk_read_bytes_per_sec(const MetricBaseline& b) {
    return suggest_throughput_rule(b,
                                   "disk_read_high",
                                   "disk.read_bytes_per_sec",
                                   /*min_activity_mb*/ 1.0,
                                   /*min_floor_mb*/    50.0);
}

std::string suggest_rule_disk_write_bytes_per_sec(const MetricBaseline& b) {
    return suggest_throughput_rule(b,
                                   "disk_write_high",
                                   "disk.write_bytes_per_sec",
                                   /*min_activity_mb*/ 1.0,
                                   /*min_floor_mb*/    50.0);
}

std::string suggest_rule_net_rx_bytes_per_sec(const MetricBaseline& b) {
    return suggest_throughput_rule(b,
                                   "net_rx_high",
                                   "net.rx_bytes_per_sec",
                                   /*min_activity_mb*/ 0.1,
                                   /*min_floor_mb*/    5.0);
}

std::string suggest_rule_net_tx_bytes_per_sec(const MetricBaseline& b) {
    return suggest_throughput_rule(b,
                                   "net_tx_high",
                                   "net.tx_bytes_per_sec",
                                   /*min_activity_mb*/ 0.1,
                                   /*min_floor_mb*/    5.0);
}

std::string suggest_rules_for_samples(const Sample* s, size_t n, const double* w) {
    std::ostringstream os;
    os << "-- AI-suggested rules (Tier A, local heuristics). Review before using.\n"
       << "-- Generated from " << n << " samples.\n\n";

    if (s == nullptr || n == 0) return os.str();

    const double cpu_count_est = static_cast<double>(newest_cpu_count(s, n));

    os << suggest_rule_cpu_total_percent       (compute_cpu_total_percent_stats       (s, n, w));
    os << suggest_rule_mem_available_percent   (compute_mem_available_percent_stats   (s, n, w));
    os << suggest_rule_swap_used_percent       (compute_swap_used_percent_stats       (s, n, w));
    os << suggest_rule_load_1m                 (compute_load_1m_stats                 (s, n, w), cpu_count_est);
    os << suggest_rule_disk_read_bytes_per_sec (compute_disk_read_bytes_per_sec_stats (s, n, w));
    os << suggest_rule_disk_write_bytes_per_sec(compute_disk_write_bytes_per_sec_stats(s, n, w));
    os << suggest_rule_net_rx_bytes_per_sec    (compute_net_rx_bytes_per_sec_stats    (s, n, w));
    os << suggest_rule_net_tx_bytes_per_sec    (compute_net_tx_bytes_per_sec_stats    (s, n, w));
    return os.str();
}

int check_suggested_rules(const std::string& lua, std::string* error, const CheckOptions* opt) {
    const CheckOptions defaults;
    if (opt == nullptr) opt = &defaults;
    LuaEngine e;
    if (e.init(opt->enable_exec) != 0) {
        if (error != nullptr) *error = "the rules engine did not start";
        return -1;
    }
    if (opt->instruction_limit != 0 || opt->memory_bytes != 0) {
        e.set_limits(opt->instruction_limit != 0 ? opt->instruction_limit : e.instruction_limit(),
                     opt->memory_bytes != 0 ? opt->memory_bytes : e.memory_limit());
    }
    if (!opt->exec_allow.empty()) e.set_exec_allowlist(opt->exec_allow);
    // The reply's file-scope statements run as the check loads it: a
    // print() landed on stdout ahead of the rules file (which then did
    // not load), an alert() logged an alert, and an escalate("L3") was
    // refused (the scratch engine knew no level names). Both are
    // silenced for the check, and the levels are known.
    e.set_level_names(opt->level_names);
    if (e.load_string("print = function() end\nalert = function() end\n") != 0) {
        if (error != nullptr) *error = e.last_error();
        return -1;
    }
    if (e.load_buffer(lua, "=suggested rules") != 0) {
        if (error != nullptr) *error = e.last_error();
        return -1;
    }
    return e.rule_count();
}

size_t suggestion_control_byte(const std::string& doc) {
    for (size_t i = 0; i < doc.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(doc[i]);
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return i;
    }
    return std::string::npos;
}

bool suggestion_names_exec(const std::string& doc) {
    const auto name_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    };
    for (size_t at = doc.find("exec"); at != std::string::npos; at = doc.find("exec", at + 4)) {
        const bool starts = at == 0 || !name_char(doc[at - 1]);
        const bool ends   = at + 4 >= doc.size() || !name_char(doc[at + 4]);
        if (starts && ends) return true;
    }
    return false;
}

std::vector<Sample> merge_l2_l1(std::vector<Sample> l2, const std::vector<Sample>& l1,
                                uint64_t l1_ns) {
    const auto by_time = [](const Sample& a, const Sample& b) {
        return a.timestamp_nanos < b.timestamp_nanos;
    };
    std::sort(l2.begin(), l2.end(), by_time);
    // Which L1 records to keep, found against the L2 records as sorted;
    // then the L2 records are moved, not copied, into the result.
    std::vector<const Sample*> keep;
    keep.reserve(l1.size());
    for (const auto& r : l1) {
        if (r.folded) {
            // The window (lo, hi], as TierAggregator folds it.
            const uint64_t hi = r.timestamp_nanos;
            const uint64_t lo = hi > l1_ns ? hi - l1_ns : 0;
            const auto it = std::upper_bound(l2.begin(), l2.end(), lo,
                [](uint64_t t, const Sample& s) { return t < s.timestamp_nanos; });
            if (it != l2.end() && it->timestamp_nanos <= hi) continue;
        }
        keep.push_back(&r);
    }
    std::vector<Sample> out = std::move(l2);
    out.reserve(out.size() + keep.size());
    for (const Sample* r : keep) out.push_back(*r);
    std::sort(out.begin(), out.end(), by_time);
    return out;
}

} // namespace budyk
