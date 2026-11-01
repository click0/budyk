// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>

namespace budyk {

/// Transpile a "simple YAML rules" document into a Lua snippet that
/// drives the existing watch() builtin. Each YAML item becomes one
/// `watch("name", { when = function() return <expr> end, ... })` call;
/// the rest of the engine stays Lua-only.
///
/// Accepted top-level shapes:
///   - a bare sequence of rule mappings
///   - a mapping with a `rules:` key whose value is the same sequence
///
/// Per-rule keys (required marked *):
///     name*        string  — rule identifier
///     when*        string  — Lua boolean expression (spliced verbatim)
///     for_ticks    int     — consecutive hits before firing (default 1)
///     cooldown     int     — ticks to wait after firing (default = for_ticks)
///     severity     string  — info / warning / critical, any case (default warning)
///     action       string  — alert (default), log, or escalate: the alert
///                            and `escalate` to L3, as `{ alert, escalate }`
///     message      string  — alert body (default = name)
///
/// Anything not in this list is ignored. The `when` expression is NOT
/// validated by us — Lua's parser surfaces syntax errors at load time
/// like it does for any rules.lua. The expression runs in the same
/// sandbox as native Lua rules, so the YAML format adds no privilege.
///
/// Returns 0 on success; negative on a parse or I/O failure or a
/// malformed rule (a required key missing, a severity or action that is
/// not one of the above, a `for_ticks` that is not a whole number),
/// with `error` (optional) saying which rule and what: "rule 'hot_cpu':
/// severity 'Fatal' is not info, warning or critical", "YAML: did not
/// find expected key at line 4". Every such failure used to read "YAML
/// parse error", and an unknown severity became warning without a word.
int yaml_rules_to_lua      (const char* yaml_text, std::string* lua_out,
                            std::string* error = nullptr);
int yaml_rules_to_lua_file (const char* path,      std::string* lua_out,
                            std::string* error = nullptr);

} // namespace budyk
