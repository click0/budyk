// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "ai/baseline.h"
#include "core/sample.h"

#include <string>

namespace budyk {

// Tier A rule suggestions — turn MetricBaseline statistics into Lua
// `watch()` calls (spec §3.7.1 step 3). Arithmetic + templates, no
// ML, no network.
//
// Per-metric generators return either an empty string (no rule
// warranted — insufficient data, or metric never active) or a
// complete Lua snippet with rationale comments.
std::string suggest_rule_cpu_total_percent       (const MetricBaseline& b);
std::string suggest_rule_mem_available_percent   (const MetricBaseline& b);
std::string suggest_rule_swap_used_percent       (const MetricBaseline& b);
std::string suggest_rule_load_1m                 (const MetricBaseline& b, double cpu_count);
std::string suggest_rule_disk_read_bytes_per_sec (const MetricBaseline& b);
std::string suggest_rule_disk_write_bytes_per_sec(const MetricBaseline& b);
std::string suggest_rule_net_rx_bytes_per_sec    (const MetricBaseline& b);
std::string suggest_rule_net_tx_bytes_per_sec    (const MetricBaseline& b);

// Extracts baselines from a Sample array and returns the full
// suggested-rules.lua document (header + each warranted rule).
std::string suggest_rules_for_samples(const Sample* s, size_t n);

} // namespace budyk
