// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>

namespace budyk {

// Transpile a "simple YAML rules" document into a Lua snippet that
// drives the existing watch() builtin. Each YAML item becomes one
// `watch("name", { when = function() return <expr> end, ... })` call;
// the rest of the engine stays Lua-only.
//
// Accepted top-level shapes:
//   - a bare sequence of rule mappings
//   - a mapping with a `rules:` key whose value is the same sequence
//
// Per-rule keys (required marked *):
//     name*        string  — rule identifier
//     when*        string  — Lua boolean expression (spliced verbatim)
//     for_ticks    int     — consecutive hits before firing (default 1)
//     cooldown     int     — ticks to wait after firing (default = for_ticks)
//     severity     string  — info / warning / critical (default warning)
//     action       string  — "alert" or "log"          (default alert)
//     message      string  — alert body (default = name)
//
// Anything not in this list is ignored. The `when` expression is NOT
// validated by us — Lua's parser surfaces syntax errors at load time
// like it does for any rules.lua. The expression runs in the same
// sandbox as native Lua rules, so the YAML format adds no privilege.
//
// Returns 0 on success; negative on parse / IO failure or a malformed
// rule (missing required key, oversized rule body).
int yaml_rules_to_lua      (const char* yaml_text, std::string* lua_out);
int yaml_rules_to_lua_file (const char* path,      std::string* lua_out);

} // namespace budyk
