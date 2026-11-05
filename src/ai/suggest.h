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
/// warranted — insufficient data, or metric never active), a complete
/// Lua snippet with rationale comments, or a comment alone when the
/// template's threshold lands inside what the host does every day (a
/// CPU p99 at or above the 95 % cap, swap at 96 %, memory under the 5 %
/// floor): such a rule held all the time, and was written.
std::string suggest_rule_cpu_total_percent       (const MetricBaseline& b);
std::string suggest_rule_mem_available_percent   (const MetricBaseline& b);
std::string suggest_rule_swap_used_percent       (const MetricBaseline& b);
std::string suggest_rule_load_1m                 (const MetricBaseline& b, double cpu_count);
std::string suggest_rule_disk_read_bytes_per_sec (const MetricBaseline& b);
std::string suggest_rule_disk_write_bytes_per_sec(const MetricBaseline& b);
std::string suggest_rule_net_rx_bytes_per_sec    (const MetricBaseline& b);
std::string suggest_rule_net_tx_bytes_per_sec    (const MetricBaseline& b);

/// Extracts baselines from a Sample array and returns the full
/// suggested-rules.lua document (header + each warranted rule). With
/// `weights` (`n` of them, sample_weights()) the statistics weigh each
/// sample by the time it stands for.
std::string suggest_rules_for_samples(const Sample* s, size_t n, const double* weights = nullptr);

/// What the check engine is set up with: what `budyk serve` reads from
/// the config (serve.cpp's init_engine). Without one the check ran with
/// exec() on, no allowlist, the default limits and the built-in levels
/// alone, so a reply that called exec() passed and then failed to load
/// in a daemon with exec off ("exec is disabled ... rules=0").
struct CheckOptions {
    bool                     enable_exec       = true;
    std::vector<std::string> exec_allow;                     ///< empty: any
    uint64_t                 instruction_limit = 0;          ///< 0: the engine's default
    size_t                   memory_bytes      = 0;          ///< 0: the engine's default
    std::vector<std::string> level_names = {"L1", "L2", "L3"};
};

/// Loads `lua` as `budyk serve` loads a rules file, in a scratch engine
/// (the rules sandbox and its limits, from `opt` when given), and
/// returns how many watch() rules it registers, or -1 with the Lua error
/// in `error` (spec §3.7.1: a suggestion is checked before it is
/// written). exec() may be named in an action: while a file loads it
/// runs nothing. freeze() stays off: a call to it while the file loads
/// would stop the process. The text is loaded whole: a NUL byte in it
/// is Lua's error, not the end of what is checked.
int check_suggested_rules(const std::string& lua, std::string* error,
                          const CheckOptions* opt = nullptr);

/// The offset of the first byte of `doc` that is a control character
/// other than tab, LF and CR, or std::string::npos. A reply with one is
/// refused: a NUL in a `--` comment hid the code after it from the check
/// (the check read a C string) while Lua's lexer skipped it and loaded
/// that code; what else such a byte does to a text is anyone's guess.
size_t suggestion_control_byte(const std::string& doc);

/// True when `doc` names `exec` as a token (not part of a longer name:
/// `execute`, `my_exec`), whatever follows it: `exec(`, `exec {`,
/// `exec "..."`, `exec[[...]]`, `local run = exec`. The note that a
/// suggestion calls exec() matched `exec(` and `exec (` alone.
bool suggestion_names_exec(const std::string& doc);

/// The samples suggest-rules reads (spec §3.7.1: Tier 2 and Tier 3): the
/// L2 ring's records and the L1 ring's, oldest first, each stretch once.
/// A folded L1 record (`Sample::folded`) whose window, the `l1_ns` up to
/// its timestamp, has an L2 record is left out, as `TierManager::query_all`
/// does; a raw one is kept, the tick after an L2 stretch too (its window
/// ends that stretch, and no other record has it). `l2` is moved into
/// the result (a year's rings were held three times over).
std::vector<Sample> merge_l2_l1(std::vector<Sample> l2, const std::vector<Sample>& l1,
                                uint64_t l1_ns);

} // namespace budyk
