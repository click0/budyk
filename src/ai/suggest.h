// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "ai/baseline.h"
#include "core/sample.h"

#include <cstdint>
#include <string>
#include <vector>

namespace budyk {

/// Tier A rule suggestions — turn MetricBaseline statistics into Lua
/// `watch()` calls (spec §3.7.1 step 3). Arithmetic + templates, no
/// ML, no network.
///
/// Per-metric generators return either an empty string (no rule
/// warranted — insufficient data, or metric never active) or a
/// complete Lua snippet with rationale comments.
std::string suggest_rule_cpu_total_percent       (const MetricBaseline& b);
std::string suggest_rule_mem_available_percent   (const MetricBaseline& b);
std::string suggest_rule_swap_used_percent       (const MetricBaseline& b);
std::string suggest_rule_load_1m                 (const MetricBaseline& b, double cpu_count);
std::string suggest_rule_disk_read_bytes_per_sec (const MetricBaseline& b);
std::string suggest_rule_disk_write_bytes_per_sec(const MetricBaseline& b);
std::string suggest_rule_net_rx_bytes_per_sec    (const MetricBaseline& b);
std::string suggest_rule_net_tx_bytes_per_sec    (const MetricBaseline& b);

/// Extracts baselines from a Sample array and returns the full
/// suggested-rules.lua document (header + each warranted rule).
std::string suggest_rules_for_samples(const Sample* s, size_t n);

/// The samples suggest-rules reads (spec §3.7.1: Tier 2 and Tier 3): the
/// L2 ring's records and the L1 ring's, oldest first, each stretch once.
/// A folded L1 record (`Sample::folded`) whose window, the `l1_ns` up to
/// its timestamp, has an L2 record is left out, as `TierManager::query_all`
/// does; a raw one is kept, the tick after an L2 stretch too (its window
/// ends that stretch, and no other record has it).
std::vector<Sample> merge_l2_l1(std::vector<Sample> l2, const std::vector<Sample>& l1,
                                uint64_t l1_ns);

} // namespace budyk
