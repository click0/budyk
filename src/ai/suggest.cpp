// SPDX-License-Identifier: BSD-3-Clause
#include "ai/suggest.h"

#include "ai/baseline.h"
#include "core/sample.h"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace budyk {

namespace {

std::string fmt_rule(const char* name,
                     const char* rationale,
                     const char* severity,
                     const std::string& when_expr) {
    std::ostringstream os;
    os << "-- " << rationale << '\n'
       << "watch(\"" << name << "\", {\n"
       << "  when     = function() return " << when_expr << " end,\n"
       << "  severity = \"" << severity << "\",\n"
       << "  action   = alert,\n"
       << "})\n\n";
    return os.str();
}

std::string fmt_num(double v, int prec = 1) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

} // namespace

std::string suggest_rule_cpu_total_percent(const MetricBaseline& b) {
    if (b.n == 0) return {};

    // Spec §3.7.1: p99 at 45% → ~85% (~2× headroom).
    // Generalize: threshold = max(60, ceil(p99 + 15)), capped at 95.
    double t = std::ceil(b.p99 + 15.0);
    if (t < 60.0) t = 60.0;
    if (t > 95.0) t = 95.0;

    std::ostringstream rat;
    rat << "Tier A: cpu.total_percent p99=" << fmt_num(b.p99)
        << "%, max=" << fmt_num(b.max) << "% over " << b.n << " samples";
    std::ostringstream cond;
    cond << "cpu.total_percent > " << fmt_num(t, 0);
    return fmt_rule("high_cpu", rat.str().c_str(), "warning", cond.str());
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
    std::ostringstream cond;
    cond << "mem.available_percent < " << fmt_num(t, 0);
    return fmt_rule("memory_low", rat.str().c_str(), severity, cond.str());
}

std::string suggest_rule_swap_used_percent(const MetricBaseline& b) {
    if (b.n == 0)     return {};
    if (b.max < 1.0)  return {};     // swap never used — no rule warranted

    double t = std::ceil(b.p99 * 2.0);
    if (t < 50.0) t = 50.0;
    if (t > 90.0) t = 90.0;

    std::ostringstream rat;
    rat << "Tier A: swap.used_percent p99=" << fmt_num(b.p99)
        << "%, max=" << fmt_num(b.max) << "% over " << b.n << " samples";
    std::ostringstream cond;
    cond << "swap.used_percent > " << fmt_num(t, 0);
    return fmt_rule("swap_pressure", rat.str().c_str(), "warning", cond.str());
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
    return fmt_rule("load_high", rat.str().c_str(), "warning", cond.str());
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

    std::ostringstream rat;
    rat << "Tier A: " << metric_expr
        << " p99=" << fmt_bps(b.p99)
        << ", max=" << fmt_bps(b.max)
        << " over " << b.n << " samples";
    std::ostringstream cond;
    cond << metric_expr << " > " << fmt_num(t, 0)
         << "  -- " << fmt_bps(t);
    return fmt_rule(rule_name, rat.str().c_str(), "warning", cond.str());
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

std::string suggest_rules_for_samples(const Sample* s, size_t n) {
    std::ostringstream os;
    os << "-- AI-suggested rules (Tier A, local heuristics). Review before using.\n"
       << "-- Generated from " << n << " samples.\n\n";

    if (s == nullptr || n == 0) return os.str();

    const double cpu_count_est = static_cast<double>(s[n - 1].cpu.count);

    os << suggest_rule_cpu_total_percent       (compute_cpu_total_percent_stats       (s, n));
    os << suggest_rule_mem_available_percent   (compute_mem_available_percent_stats   (s, n));
    os << suggest_rule_swap_used_percent       (compute_swap_used_percent_stats       (s, n));
    os << suggest_rule_load_1m                 (compute_load_1m_stats                 (s, n), cpu_count_est);
    os << suggest_rule_disk_read_bytes_per_sec (compute_disk_read_bytes_per_sec_stats (s, n));
    os << suggest_rule_disk_write_bytes_per_sec(compute_disk_write_bytes_per_sec_stats(s, n));
    os << suggest_rule_net_rx_bytes_per_sec    (compute_net_rx_bytes_per_sec_stats    (s, n));
    os << suggest_rule_net_tx_bytes_per_sec    (compute_net_tx_bytes_per_sec_stats    (s, n));
    return os.str();
}

} // namespace budyk
