// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/alert_event.h"
#include "core/sample.h"
#include "core/sample_c.h"
#include "rules/alert.h"
#include "rules/worker.h"
#include "security/file_watcher.h"

#include <cstdint>
#include <string>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace budyk {

struct LuaRule {
    std::string name;
    int         when_ref;          ///< LUA_REGISTRYINDEX ref
    /// LUA_REGISTRYINDEX ref to the actions run in order when the rule
    /// fires: a Lua sequence of "alert", "log", "escalate" or functions
    /// (see watch() in lua_stdlib.cpp).
    int         actions_ref;
    AlertSeverity severity;        ///< used by "alert"
    std::string   message;         ///< used by "alert" / "log"
    uint64_t    fire_count;

    int         for_ticks;         ///< consecutive true evaluations needed to fire
    int         cooldown_ticks;    ///< ticks to skip after firing (default: for_ticks)
    int         consecutive_hits;  ///< runtime: current streak of true evaluations
    int         cooldown_remaining;///< runtime: ticks left before rule becomes active again
    std::string last_error;        ///< last logged when/action error, to log each one once
    std::string last_skip;         ///< last logged "not collected at this level", likewise
};

/// A request made by escalate() in a rule: keep `level` active for
/// `seconds` from now. The serve loop hands these to the scheduler.
struct Escalation {
    std::string level;
    int         seconds;
};

/// Embedded Lua 5.4 rule engine (spec §3.6).
/// Sandbox: opens only _G (base) + math + string + table; removes
/// dofile / loadfile / load / require from globals.
class LuaEngine {
public:
    LuaEngine() = default;
    /// Owns the lua_State: closed here if shutdown() was not called (an
    /// early return, a test that forgot), so it never leaks.
    ~LuaEngine() { shutdown(); }
    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    int  init(bool enable_exec);
    void shutdown();

    int  load_string(const char* code);
    int  load_file  (const char* path);

    /// Lua error text from the last failed load_string / load_file, e.g.
    /// "rules.lua:2: watch(x): 'action' must be a function, ...".
    const std::string& last_error() const;

    /// Persist / restore per-rule runtime state (cooldown_remaining,
    /// consecutive_hits, fire_count) keyed by rule name. Purpose: a
    /// daemon restart shouldn't re-fire a rule that was mid-cooldown,
    /// which would otherwise produce an alert-storm. load_state must be
    /// called *after* the rules are loaded (it matches saved entries to
    /// rules by name; unknown entries are dropped, unmatched rules keep
    /// their defaults). Cooldown is stored as a tick count — a long
    /// downtime therefore does NOT decrement it, which errs on the safe
    /// side (stay quiet a little longer rather than storm).
    ///   save_state: 0 on success, -errno on write failure.
    ///   load_state: 0 on success or missing file (fresh install).
    int  save_state(const char* path) const;
    int  load_state(const char* path);

    /// Binds `s` as read-only Lua globals and calls every rule's `when()`.
    /// Returns the number of rules that fired, or -1 if not initialised.
    /// `detail` (optional): the per-entity detail of `s`, for the rules'
    /// cpu.cores, disk.devices, net.interfaces and fs.mounts.
    /// A when() that reads a group or list `s` lacks (see lua_bindings.h)
    /// skips the rule for this tick: its sustain count stays where it was
    /// (spec §3.6.5). The first skip of each kind is logged, unless it was
    /// only a rate's first read. A bind that fails (the rules hold the
    /// state at rules.limits.memory_mb) evaluates nothing and returns 0;
    /// its error is logged once.
    int  eval_tick(const Sample& s, const budyk_detail_c* detail = nullptr);

    /// Custom-level entry conditions (collection.levels[].when). The
    /// expression is compiled as `return (<expr>)` in the rules sandbox.
    /// Add after init(), and again after a reload. Returns 0, -1 if the
    /// engine isn't initialised, -2 on a compile error (see last_error()).
    int  add_level_condition(uint8_t level_id, const std::string& expr);
    /// Binds `s` and evaluates every condition. *active is cleared, then
    /// gets the id of each level whose expression is true. A condition
    /// that raises counts as false and is logged once per distinct error;
    /// so does one that reads what `s` lacks, logged as skipped (unless
    /// only a rate's first read).
    void eval_level_conditions(const Sample& s, std::vector<uint8_t>* active,
                               const budyk_detail_c* detail = nullptr);

    /// A level condition and what it last logged: an error, or a skip for
    /// reading what the level does not collect.
    struct LevelCondition {
        uint8_t     level_id;
        int         ref;
        std::string last_error;
        std::string last_skip;
    };
    const std::vector<LevelCondition>& level_conditions() const;

    /// Level names escalate() accepts ("L1".."L3" and the custom names).
    void set_level_names(std::vector<std::string> names);
    bool is_level_name(const char* name) const;
    /// Called by the escalate() binding.
    void push_escalation(const char* level, int seconds);
    /// escalate() requests made since the last call, oldest first.
    std::vector<Escalation> take_escalations();

    /// An alert (a rule's "alert" action, or alert() from Lua): logged,
    /// queued for take_alerts() and sent to every channel. Returns the
    /// number of channels it was queued for.
    int raise_alert(AlertSeverity sev, const std::string& rule, const std::string& message);
    /// The alerts raised since the last call, oldest first: the serve loop
    /// pushes them to the dashboards and keeps them for /api/alerts. At
    /// most kMaxPendingAlerts wait; the rest are counted, not kept.
    static constexpr size_t kMaxPendingAlerts = 256;
    std::vector<AlertEvent> take_alerts();
    uint64_t                alerts_dropped() const;

    /// True while load_string() / load_file() runs the rules file. exec()
    /// called then returns the command as an action instead of running it
    /// (spec §3.6.2: action = { alert, exec("/path") }).
    bool loading() const;

    int  rule_count()      const;
    int  last_fire_count() const;
    bool exec_enabled()    const;
    bool freeze_enabled()  const;

    /// Execution limits (spec §3.6: a runaway rule must not stop the
    /// daemon). Every call into Lua — a rule's when() or action, a level
    /// condition, a file being loaded — may run at most `instructions`
    /// VM instructions, and the whole Lua state may hold at most
    /// `memory_bytes`. An overrun raises a Lua error in that call, which
    /// the call site logs like any other rule error; the other rules and
    /// the next tick are unaffected. A C function such as string.rep is
    /// one instruction, which is what the memory limit is for.
    ///
    /// Instructions are counted rather than time so the limit means the
    /// same on a slow machine, under a sanitizer, or in a test. Takes
    /// effect at once, before or after init().
    static constexpr uint64_t kDefaultInstructionLimit = 1'000'000;
    static constexpr size_t   kDefaultMemoryLimit      = 16u << 20;   ///< 16 MiB
    void     set_limits(uint64_t instructions, size_t memory_bytes);
    uint64_t instruction_limit() const;
    size_t   memory_limit()      const;
    /// Bytes currently held by the Lua state (0 when not initialised).
    size_t   memory_used()       const;

    /// Charge `n` units of work done in C (a pattern match, table.move)
    /// to the instruction limit of the engine that owns `L`, as the count
    /// hook charges VM instructions; raises once the limit is spent (so
    /// charge(L, 0) raises if it already is). Past the limit every
    /// further instruction raises too, so a pcall() in the rule cannot
    /// carry on.
    static void charge(lua_State* L, uint64_t n);

    /// exec() hardening: when the allowlist is non-empty, exec() rejects
    /// any argv[0] that is not exactly one of the listed absolute paths.
    /// An empty allowlist allows any absolute-path command (still subject
    /// to the no-traversal / must-be-absolute checks in l_exec).
    void set_exec_allowlist(std::vector<std::string> paths);
    /// The allowlist with every entry passed through realpath(3) when it
    /// was set (an entry that does not resolve is kept as given). exec()
    /// compares the command's realpath against this, so "/usr/bin//x",
    /// "/usr/bin/./x" and a symlink to the allowed binary match, while
    /// an allowed path that is later replaced by a symlink to something
    /// else does not: it resolved to the original when it was allowed.
    const std::vector<std::string>& exec_allowlist_resolved() const;
    const std::vector<std::string>& exec_allowlist() const;

    /// freeze() / unfreeze() gate. Disabled by default; admins opt in
    /// via --enable-freeze (CLI) or rules.freeze.enabled (YAML). The
    /// allowlist matches the target PID's process name (`comm`) against
    /// any entry — empty list ⇒ no name-based gate (still requires the
    /// engine-wide enabled flag).
    void set_freeze_enabled(bool enable);
    void set_freeze_allowlist(std::vector<std::string> names);
    const std::vector<std::string>& freeze_allowlist() const;

    /// Mutable reference — call sites add channels at config-load time
    /// and l_alert() pulls the dispatcher when a rule fires.
    AlertDispatcher&       alerts();
    const AlertDispatcher& alerts() const;

    /// The thread exec() runs commands on (see l_exec). Like alerts(),
    /// it outlives shutdown(): a reload must not wait for a running
    /// command, and a command must not be lost to one.
    Worker&                exec_worker();

    /// File-watch state, projected to the `files` Lua global. cmd_serve
    /// calls set_file_state() right after FileWatcher::poll on every
    /// tick; eval_tick re-binds before running rules so the per-tick
    /// `tampered` flag is correct.
    void                  set_file_state(const FileWatchState& s);
    bool                  has_file_state() const;
    const FileWatchState& file_state() const;

    const std::vector<LuaRule>& rules() const;

    /// Called by the watch() C binding. Public so the binding can reach
    /// the engine via the Lua registry without any friendship gymnastics.
    void add_rule(const std::string& name, int when_ref, int actions_ref,
                  AlertSeverity severity, const std::string& message,
                  int for_ticks, int cooldown_ticks);

private:
    /// Runs a rule's actions when it fires (see LuaRule::actions_ref).
    void run_actions(LuaRule* r);

    /// Lua allocator (lua_Alloc) that enforces memory_limit_, and the
    /// count hook that enforces instruction_limit_. Both find the engine
    /// through the allocator's userdata (lua_getallocf), so neither
    /// needs a registry lookup on the hot path.
    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static void  count_hook(lua_State* L, lua_Debug* ar);
    static LuaEngine* engine_of(lua_State* L);
    /// Raises the limit's error and sets the hook to fire on every
    /// instruction until the next begin_call().
    static int   raise_limit(lua_State* L, const LuaEngine* self);
    /// Resets the instruction count and the hook's interval; called
    /// before every entry into Lua.
    void begin_call();
    /// Binds `s` (and the file-watch state) in a protected call. False,
    /// logged once per distinct error, when the bind raised: the tick's
    /// rules and level conditions are then not evaluated.
    bool bind(const Sample& s, const budyk_detail_c* detail);

    lua_State*               L_               = nullptr;
    uint64_t                 instruction_limit_ = kDefaultInstructionLimit;
    uint64_t                 instructions_used_ = 0;
    size_t                   memory_limit_      = kDefaultMemoryLimit;
    size_t                   memory_used_       = 0;
    std::vector<LuaRule>     rules_;
    bool                     exec_enabled_    = false;
    int                      last_fire_count_ = 0;
    std::vector<std::string> exec_allowlist_;
    std::vector<std::string> exec_allowlist_resolved_;
    bool                     freeze_enabled_  = false;
    std::vector<std::string> freeze_allowlist_;
    AlertDispatcher          alerts_;
    Worker                   exec_worker_{"exec"};
    std::string              last_error_;
    std::string              bind_error_;
    std::vector<LevelCondition> level_conditions_;
    std::vector<std::string>    level_names_;
    std::vector<Escalation>     escalations_;
    std::vector<AlertEvent>     alert_events_;
    uint64_t                    alerts_dropped_ = 0;
    bool                        loading_        = false;
    bool                     has_file_state_  = false;
    FileWatchState           file_state_;
};

} // namespace budyk
