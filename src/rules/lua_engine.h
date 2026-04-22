// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace budyk {

struct LuaRule {
    std::string name;
    int         when_ref;          // LUA_REGISTRYINDEX ref
    int         action_ref;        // LUA_REGISTRYINDEX ref, or LUA_REFNIL
    std::string action_tag;        // "alert" / "log" / "" if action_ref is set
    uint64_t    fire_count;

    int         for_ticks;         // consecutive true evaluations needed to fire
    int         cooldown_ticks;    // ticks to skip after firing (default: for_ticks)
    int         consecutive_hits;  // runtime: current streak of true evaluations
    int         cooldown_remaining;// runtime: ticks left before rule becomes active again
};

// Embedded Lua 5.4 rule engine (spec §3.6).
// Sandbox: opens only _G (base) + math + string + table; removes
// dofile / loadfile / load / require from globals.
class LuaEngine {
public:
    int  init(bool enable_exec);
    void shutdown();

    int  load_string(const char* code);
    int  load_file  (const char* path);

    // Binds `s` as read-only Lua globals and calls every rule's `when()`.
    // Returns the number of rules that fired, or -1 if not initialised.
    int  eval_tick(const Sample& s);

    int  rule_count()      const;
    int  last_fire_count() const;
    bool exec_enabled()    const;

    // exec() hardening: when the allowlist is non-empty, exec() rejects
    // any argv[0] that is not exactly one of the listed absolute paths.
    // An empty allowlist allows any absolute-path command (still subject
    // to the no-traversal / must-be-absolute checks in l_exec).
    void set_exec_allowlist(std::vector<std::string> paths);
    const std::vector<std::string>& exec_allowlist() const;

    const std::vector<LuaRule>& rules() const;

    // Called by the watch() C binding. Public so the binding can reach
    // the engine via the Lua registry without any friendship gymnastics.
    void add_rule(const std::string& name, int when_ref, int action_ref,
                  const std::string& action_tag,
                  int for_ticks, int cooldown_ticks);

private:
    lua_State*               L_               = nullptr;
    std::vector<LuaRule>     rules_;
    bool                     exec_enabled_    = false;
    int                      last_fire_count_ = 0;
    std::vector<std::string> exec_allowlist_;
};

} // namespace budyk
