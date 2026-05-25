// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include "rules/alert.h"
#include "security/file_watcher.h"
#include "security/ssh_audit.h"

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

    // Persist / restore per-rule runtime state (cooldown_remaining,
    // consecutive_hits, fire_count) keyed by rule name. Purpose: a
    // daemon restart shouldn't re-fire a rule that was mid-cooldown,
    // which would otherwise produce an alert-storm. load_state must be
    // called *after* the rules are loaded (it matches saved entries to
    // rules by name; unknown entries are dropped, unmatched rules keep
    // their defaults). Cooldown is stored as a tick count — a long
    // downtime therefore does NOT decrement it, which errs on the safe
    // side (stay quiet a little longer rather than storm).
    //   save_state: 0 on success, -errno on write failure.
    //   load_state: 0 on success or missing file (fresh install).
    int  save_state(const char* path) const;
    int  load_state(const char* path);

    // Binds `s` as read-only Lua globals and calls every rule's `when()`.
    // Returns the number of rules that fired, or -1 if not initialised.
    int  eval_tick(const Sample& s);

    int  rule_count()      const;
    int  last_fire_count() const;
    bool exec_enabled()    const;
    bool freeze_enabled()  const;

    // exec() hardening: when the allowlist is non-empty, exec() rejects
    // any argv[0] that is not exactly one of the listed absolute paths.
    // An empty allowlist allows any absolute-path command (still subject
    // to the no-traversal / must-be-absolute checks in l_exec).
    void set_exec_allowlist(std::vector<std::string> paths);
    const std::vector<std::string>& exec_allowlist() const;

    // freeze() / unfreeze() gate. Disabled by default; admins opt in
    // via --enable-freeze (CLI) or rules.freeze.enabled (YAML). The
    // allowlist matches the target PID's process name (`comm`) against
    // any entry — empty list ⇒ no name-based gate (still requires the
    // engine-wide enabled flag).
    void set_freeze_enabled(bool enable);
    void set_freeze_allowlist(std::vector<std::string> names);
    const std::vector<std::string>& freeze_allowlist() const;

    // Mutable reference — call sites add channels at config-load time
    // and l_alert() pulls the dispatcher when a rule fires.
    AlertDispatcher&       alerts();
    const AlertDispatcher& alerts() const;

    // Latest SSH audit-log snapshot. cmd_serve calls set_ssh_audit()
    // after each periodic scan; eval_tick re-binds the `ssh_audit`
    // Lua global so rules see fresh counters every tick. Unset until
    // the first call — in which case the binding stays absent and
    // rules referencing it get a `nil` (cheaply caught by Lua).
    void                  set_ssh_audit(const SshAuditStats& s);
    bool                  has_ssh_audit() const;
    const SshAuditStats&  ssh_audit() const;

    // File-watch state, projected to the `files` Lua global. cmd_serve
    // calls set_file_state() right after FileWatcher::poll on every
    // tick; eval_tick re-binds before running rules so the per-tick
    // `tampered` flag is correct.
    void                  set_file_state(const FileWatchState& s);
    bool                  has_file_state() const;
    const FileWatchState& file_state() const;

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
    bool                     freeze_enabled_  = false;
    std::vector<std::string> freeze_allowlist_;
    AlertDispatcher          alerts_;
    bool                     has_ssh_audit_   = false;
    SshAuditStats            ssh_audit_;
    bool                     has_file_state_  = false;
    FileWatchState           file_state_;
};

} // namespace budyk
