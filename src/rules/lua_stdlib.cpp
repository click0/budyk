// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_stdlib.h"

#include "rules/exec_action.h"
#include "rules/freeze.h"
#include "rules/lua_engine.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kEngineRegKey = "budyk.engine";

budyk::LuaEngine* engine_from(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kEngineRegKey);
    auto* eng = static_cast<budyk::LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return eng;
}

int l_alert(lua_State* L);      // defined below; watch() recognises them as actions
int l_escalate(lua_State* L);

// What an action value names: a function to call, one of the built-in
// actions, or nothing valid. `alert` and `escalate` (the builtins
// themselves) mean the built-in actions, not a call with no arguments.
enum class ActionKind { Function, Alert, Log, Escalate, Invalid };
ActionKind action_kind(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
        case LUA_TFUNCTION: {
            const lua_CFunction f = lua_tocfunction(L, idx);
            if (f == l_alert)    return ActionKind::Alert;
            if (f == l_escalate) return ActionKind::Escalate;
            return ActionKind::Function;
        }
        case LUA_TSTRING: {
            const char* tag = lua_tostring(L, idx);
            if (std::strcmp(tag, "alert")    == 0) return ActionKind::Alert;
            if (std::strcmp(tag, "log")      == 0) return ActionKind::Log;
            if (std::strcmp(tag, "escalate") == 0) return ActionKind::Escalate;
            return ActionKind::Invalid;
        }
        default:
            return ActionKind::Invalid;
    }
}

// Push the action at `idx` as an entry of a rule's action list: the
// function itself, or the built-in's tag.
void push_action(lua_State* L, int idx, ActionKind k) {
    switch (k) {
        case ActionKind::Alert:    lua_pushliteral(L, "alert");    break;
        case ActionKind::Log:      lua_pushliteral(L, "log");      break;
        case ActionKind::Escalate: lua_pushliteral(L, "escalate"); break;
        default:                   lua_pushvalue(L, idx);          break;
    }
}

// opts[key] as a whole number in lo..hi, or `fallback` when absent.
// False if it is there but not such a number (a string that reads as
// one counts, as everywhere in Lua).
bool opt_count_field(lua_State* L, int tbl, const char* key, lua_Integer lo,
                     lua_Integer hi, lua_Integer fallback, lua_Integer* out) {
    lua_getfield(L, tbl, key);
    bool ok = true;
    *out = fallback;
    if (!lua_isnil(L, -1)) {
        int isnum = 0;
        const lua_Integer v = lua_tointegerx(L, -1, &isnum);
        ok = isnum != 0 && v >= lo && v <= hi;
        if (ok) *out = v;
    }
    lua_pop(L, 1);
    return ok;
}

// watch(name, opts) — registers a rule (spec §3.6.2). opts:
//   when      — required function; true fires (after for_ticks), false
//               or no value resets the streak, nil skips the tick and
//               keeps it (spec §3.6.5)
//   action    — optional, run when the rule fires; one action or a
//               sequence of them, run in order:
//                 alert / "alert"        alert(name, severity, message)
//                                        (the default)
//                 "log"                  print "[budyk] <message>"
//                 escalate / "escalate"  keep L3 on for 60 s
//                 a function             called with no arguments; this
//                                        includes exec("/path") written
//                                        in the rules file (see l_exec)
//   severity  — "info" / "warning" (default) / "critical"; "alert" only
//   message   — string; default is the rule name
//   for_ticks — consecutive true evaluations needed to fire (1 by
//               default, up to 1 000 000; 0 means 1)
//   cooldown  — ticks to skip after firing (spec §3.6.4: by default as
//               many as for_ticks; 0 for none)
// Anything else in action / severity / for_ticks / cooldown is an error
// at load time, so a misconfigured rule fails loudly instead of silently
// doing nothing.
int l_watch(lua_State* L) {
    // Rules are registered while the rules file loads. watch() from a
    // rule's when() or action ran while eval_tick() walked the rules: the
    // vector grew under it (a use-after-free), and each tick added rules.
    auto* eng = engine_from(L);
    if (eng == nullptr) {
        return luaL_error(L, "watch: engine not bound");
    }
    if (!eng->loading()) {
        return luaL_error(L, "watch() registers a rule while the rules file loads; "
                             "it cannot be called from a rule");
    }
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    // Validate every field before creating any registry ref or C++ object:
    // luaL_error longjmps, which would leak a ref and skip destructors.
    // Each field is read once and kept on the stack: opts may have an
    // __index that answers differently the second time.
    lua_getfield(L, 2, "when");
    const int when_idx = lua_gettop(L);
    if (!lua_isfunction(L, when_idx)) {
        return luaL_error(L, "watch(%s): 'when' must be a function", name);
    }

    lua_getfield(L, 2, "action");
    const int action_idx  = lua_gettop(L);
    const int action_type = lua_type(L, action_idx);
    lua_Integer n_actions = 1;
    if (action_type == LUA_TTABLE) {
        n_actions = static_cast<lua_Integer>(lua_rawlen(L, action_idx));
        if (n_actions == 0) {
            return luaL_error(L, "watch(%s): 'action' is an empty list", name);
        }
        for (lua_Integer i = 1; i <= n_actions; ++i) {
            lua_rawgeti(L, action_idx, i);
            const bool ok = action_kind(L, -1) != ActionKind::Invalid;
            lua_pop(L, 1);
            if (!ok) {
                return luaL_error(L,
                    "watch(%s): action[%d] must be a function, alert, escalate, "
                    "\"alert\", \"log\" or \"escalate\"", name, static_cast<int>(i));
            }
        }
    } else if (action_type != LUA_TNIL && action_kind(L, action_idx) == ActionKind::Invalid) {
        return luaL_error(L,
            "watch(%s): 'action' must be a function, alert, escalate, \"alert\", "
            "\"log\", \"escalate\" or a list of them", name);
    }

    budyk::AlertSeverity severity = budyk::AlertSeverity::Warning;
    lua_getfield(L, 2, "severity");
    const int  sev_type = lua_type(L, -1);
    const bool sev_ok   = sev_type == LUA_TNIL ||
        (sev_type == LUA_TSTRING &&
         budyk::parse_severity(lua_tostring(L, -1), &severity));
    lua_pop(L, 1);
    if (!sev_ok) {
        return luaL_error(L,
            "watch(%s): 'severity' must be \"info\", \"warning\" or \"critical\"", name);
    }

    lua_getfield(L, 2, "message");
    const int msg_idx  = lua_gettop(L);
    const int msg_type = lua_type(L, msg_idx);
    if (msg_type != LUA_TNIL && msg_type != LUA_TSTRING) {
        return luaL_error(L, "watch(%s): 'message' must be a string", name);
    }

    // for_ticks = 0 has always meant 1; it still does, cooldown default
    // included.
    lua_Integer for_ticks = 1, cooldown_ticks = 0;
    if (!opt_count_field(L, 2, "for_ticks", 0, 1000000, 1, &for_ticks)) {
        return luaL_error(L, "watch(%s): 'for_ticks' must be a whole number from 1 to 1000000",
                          name);
    }
    if (for_ticks == 0) for_ticks = 1;
    if (!opt_count_field(L, 2, "cooldown", 0, 1000000, for_ticks, &cooldown_ticks)) {
        return luaL_error(L, "watch(%s): 'cooldown' must be a whole number from 0 to 1000000",
                          name);
    }

    // The refs and the action list allocate, and so may raise: they come
    // before the C++ objects. (A ref made before a later one fails is the
    // only leak, and only when the state is out of memory.)
    lua_createtable(L, static_cast<int>(n_actions), 0);          // the action list
    if (action_type == LUA_TNIL) {
        lua_pushliteral(L, "alert");
        lua_rawseti(L, -2, 1);
    } else if (action_type == LUA_TTABLE) {
        for (lua_Integer i = 1; i <= n_actions; ++i) {
            lua_rawgeti(L, action_idx, i);
            push_action(L, -1, action_kind(L, -1));
            lua_rawseti(L, -3, i);
            lua_pop(L, 1);
        }
    } else {
        push_action(L, action_idx, action_kind(L, action_idx));
        lua_rawseti(L, -2, 1);
    }
    const int actions_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, when_idx);
    const int when_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Nothing below raises.
    size_t msg_len = 0;
    const char* msg = msg_type == LUA_TSTRING ? lua_tolstring(L, msg_idx, &msg_len) : nullptr;
    const std::string message = msg != nullptr ? std::string(msg, msg_len) : std::string(name);
    eng->add_rule(name, when_ref, actions_ref, severity, message,
                  static_cast<int>(for_ticks), static_cast<int>(cooldown_ticks));
    return 0;
}

int l_alert(lua_State* L) {
    // alert(name, severity?, message?) — fires every configured channel.
    //   name      — required string (the rule name / event name)
    //   severity  — optional "info" / "warning" / "critical" (default warning)
    //   message   — optional human-readable body (default empty)
    auto* eng = engine_from(L);
    if (eng == nullptr) return 0;

    const char* name    = luaL_checkstring(L, 1);
    const char* sev_str = lua_tostring(L, 2);
    // A number reads as its text; a boolean or a table (a comparison, or
    // the sample table itself, where the string was meant) is an error,
    // not an empty message sent without a word.
    if (!lua_isnoneornil(L, 3) && !lua_isstring(L, 3)) {
        return luaL_error(L, "alert: 'message' must be a string, not %s", luaL_typename(L, 3));
    }
    size_t msg_len = 0;
    const char* msg = lua_tolstring(L, 3, &msg_len);

    // Lenient on purpose: an unknown severity falls back to warning rather
    // than failing the action mid-incident.
    budyk::AlertSeverity sev = budyk::AlertSeverity::Warning;
    budyk::parse_severity(sev_str, &sev);

    const int ok = eng->raise_alert(sev, name, msg != nullptr ? std::string(msg, msg_len) : std::string());
    lua_pushinteger(L, ok);
    return 1;
}

// escalate(level, seconds?) — keep a collection level active for
// `seconds` (default 60, 1..86400) from now: a custom level's name, or
// "L2" / "L3". The serve loop hands the request to the scheduler after the
// rules have run, so it also shortens the current sleep. Returns true;
// raises for an unknown level or a bad duration.
int l_escalate(lua_State* L) {
    const char*       level   = luaL_checkstring(L, 1);
    const lua_Integer seconds = luaL_optinteger(L, 2, 60);
    if (seconds < 1 || seconds > 86400) {
        return luaL_error(L, "escalate: seconds must be 1..86400");
    }
    auto* eng = engine_from(L);
    if (eng == nullptr) return luaL_error(L, "escalate: engine not bound");
    if (!eng->is_level_name(level)) {
        return luaL_error(L, "escalate: unknown level '%s'", level);
    }
    eng->push_escalation(level, static_cast<int>(seconds));
    lua_pushboolean(L, 1);
    return 1;
}

// What exec_impl did, for l_exec to hand back to Lua once exec_impl's
// C++ objects are gone.
struct ExecOutcome {
    enum Kind { Checked, Queued, QueueFull, Ran } kind = Checked;
    int               rc  = 0;     // Ran: exec_command()'s return
    budyk::ExecResult res{};       // Ran
};

// An argv element: a string as it is, a number as tostring() writes it
// (Lua's "%.14g", ".0" on a float that reads as an integer). Formatted
// here, not by lua_tostring(), which allocates and so may raise.
bool argv_element(lua_State* L, int idx, std::string* out) {
    switch (lua_type(L, idx)) {
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            out->assign(s, len);
            return true;
        }
        case LUA_TNUMBER: {
            char buf[64];
            if (lua_isinteger(L, idx)) {
                std::snprintf(buf, sizeof(buf), "%lld",
                              static_cast<long long>(lua_tointeger(L, idx)));
            } else {
                std::snprintf(buf, sizeof(buf), "%.14g", static_cast<double>(lua_tonumber(L, idx)));
            }
            out->assign(buf);
            if (!lua_isinteger(L, idx) && buf[std::strspn(buf, "-0123456789")] == '\0') {
                out->append(".0");
            }
            return true;
        }
        default:
            return false;
    }
}

// Does the work of exec(): 0 with *out saying what it did (Checked while
// the rules file loads: nothing run), or -1 with a message in err. It
// pushes nothing, and raises only before its first C++ object: luaL_error
// longjmps past C++ destructors, and this function owns std::string /
// std::vector objects, so l_exec raises, and builds the result table,
// only after they are gone. Raw table access keeps metamethods from
// raising in here too.
int exec_impl(lua_State* L, budyk::LuaEngine* eng, ExecOutcome* out, char* err,
              size_t err_cap) {
    // Optional second argument: a timeout in seconds, or a table
    //   { timeout = <seconds>, wait = <bool> }.
    // Default 30 s, at most a day; clamped here so a huge Lua integer
    // can't wrap through the int. Read first: pushing a key may raise
    // (out of memory), which must happen before any C++ object exists;
    // it came after the argv strings, whose destructors the raise skipped.
    int  timeout_s = 30;
    bool wait      = false;
    // 0, a negative number or a fraction (0.5 read as 0) used to mean
    // the default 30 s without a word.
    auto take_timeout = [&](int idx) -> bool {
        if (!lua_isinteger(L, idx)) return false;
        const lua_Integer t = lua_tointeger(L, idx);
        if (t < 1) return false;
        timeout_s = static_cast<int>(t < 86400 ? t : 86400);
        return true;
    };
    const char* const kBadTimeout = "exec: 'timeout' must be a whole number of seconds, 1 or more";
    if (lua_isnumber(L, 2)) {
        if (!take_timeout(2)) {
            std::snprintf(err, err_cap, "%s", kBadTimeout);
            return -1;
        }
    } else if (lua_istable(L, 2)) {
        lua_pushstring(L, "timeout");
        lua_rawget(L, 2);
        const bool timeout_ok = lua_isnil(L, -1) || take_timeout(-1);
        lua_pop(L, 1);
        if (!timeout_ok) {
            std::snprintf(err, err_cap, "%s", kBadTimeout);
            return -1;
        }
        lua_pushstring(L, "wait");
        lua_rawget(L, 2);
        wait = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }

    // Accept either exec("/path/to/cmd")          (single-arg form)
    //            or exec({"/bin/sh", "-c", "..."}) (argv table form).
    std::vector<std::string> argv_storage;
    std::string              arg;
    if (argv_element(L, 1, &arg)) {
        argv_storage.push_back(std::move(arg));
    } else if (lua_istable(L, 1)) {
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
        if (n <= 0) {
            std::snprintf(err, err_cap, "exec: empty argv table");
            return -1;
        }
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, i);
            const bool ok = argv_element(L, -1, &arg);
            lua_pop(L, 1);
            if (!ok) {
                std::snprintf(err, err_cap, "exec: argv[%d] is not a string",
                                  static_cast<int>(i));
                return -1;
            }
            argv_storage.push_back(std::move(arg));
        }
    } else {
        std::snprintf(err, err_cap, "exec: expected string or argv table");
        return -1;
    }
    // A NUL ends the string realpath() and execvp() see, not the one the
    // allowlist's name check reads: "/bin/sh\0/usr/bin/logger" resolved to
    // the BusyBox an allowed /usr/bin/logger is, was named logger, and ran
    // as sh.
    for (size_t i = 0; i < argv_storage.size(); ++i) {
        if (argv_storage[i].find('\0') != std::string::npos) {
            std::snprintf(err, err_cap, "exec: argv[%zu] holds a NUL byte", i + 1);
            return -1;
        }
    }

    // --- Hardening -----------------------------------------------------
    // Three layers of defence against an adversary turning exec() into
    // arbitrary-binary launch:
    //   1. argv[0] must be an absolute path (no PATH lookup, no relative
    //      references resolved against the daemon's cwd).
    //   2. argv[0] must not contain any '..' path segment — prevents
    //      /usr/bin/../../bin/sh-style escapes from inside a chroot.
    //   3. If the engine has a non-empty exec allowlist configured,
    //      argv[0] must exactly match one of its entries. An empty
    //      allowlist means "any absolute path under rules 1 and 2" and
    //      is the default for backwards compatibility.
    // -------------------------------------------------------------------
    const std::string& cmd = argv_storage.front();
    if (cmd.empty() || cmd.front() != '/') {
        std::snprintf(err, err_cap, "exec: argv[0] must be an absolute path");
        return -1;
    }
    // A '/..' substring is only a traversal if it sits on a path-segment
    // boundary — i.e. it's followed by '/' (middle of path) or end of string.
    auto contains_traversal = [](const std::string& s) {
        size_t pos = 0;
        while ((pos = s.find("/..", pos)) != std::string::npos) {
            const size_t end = pos + 3;
            if (end == s.size() || s[end] == '/') return true;
            pos = end;
        }
        return false;
    };
    if (contains_traversal(cmd)) {
        std::snprintf(err, err_cap, "exec: path traversal (..) forbidden in argv[0]");
        return -1;
    }
    const auto& allow = eng->exec_allowlist_resolved();
    if (!allow.empty()) {
        // The resolved path and the name as written must both match one
        // entry (see LuaEngine::exec_allowlist_resolved): a multi-call
        // binary runs the program its argv[0] names, so with /usr/bin/logger
        // allowed, /bin/sh resolving to the same BusyBox is not.
        char rbuf[PATH_MAX];
        const char* r = ::realpath(cmd.c_str(), rbuf);
        const std::string resolved = r != nullptr ? std::string(r) : cmd;
        auto base = [](const std::string& p) { return p.substr(p.find_last_of('/') + 1); };
        const auto& written = eng->exec_allowlist();
        bool found = false;
        for (size_t i = 0; i < allow.size() && i < written.size() && !found; ++i) {
            found = allow[i] == resolved && base(written[i]) == base(cmd);
        }
        if (!found) {
            std::snprintf(err, err_cap, "exec: '%s' not in allowlist", cmd.c_str());
            return -1;
        }
    }

    // While the rules file loads, exec() runs nothing: the checks above
    // pass, and the caller (l_exec) hands back the command as an action.
    if (eng->loading()) return 0;

    if (!wait) {
        // The normal case (spec §3.6: fork+exec, non-blocking). The
        // command runs on the engine's exec worker; the tick does not
        // wait for it, and its outcome goes to the log. Lua gets
        // { queued = true } — or { queued = false, error = "..." } when
        // the queue is full.
        // For the log, on one line: an argument holds anyone's bytes.
        std::string what = budyk::header_value(cmd);
        for (size_t i = 1; i < argv_storage.size(); ++i) {
            what += ' ';
            what += budyk::header_value(argv_storage[i]);
        }
        const bool queued = eng->exec_worker().post(
            [argv = std::move(argv_storage), what, timeout_s]
            (const std::atomic<bool>& cancel) {
                std::vector<const char*> ptrs;
                ptrs.reserve(argv.size() + 1);
                for (const auto& a : argv) ptrs.push_back(a.c_str());
                ptrs.push_back(nullptr);
                budyk::ExecResult res{};
                const int rc = budyk::exec_command(ptrs.data(), timeout_s, &res, &cancel);
                if (rc != 0) {
                    std::fprintf(stderr, "budyk exec: %s: could not start (rc=%d)\n",
                                 what.c_str(), rc);
                } else if (res.cancelled) {
                    std::fprintf(stderr, "budyk exec: %s: killed at shutdown after %.1f s\n",
                                 what.c_str(), res.elapsed_seconds);
                } else if (res.timed_out) {
                    std::fprintf(stderr, "budyk exec: %s: timed out after %d s, killed\n",
                                 what.c_str(), timeout_s);
                } else if (res.signal != 0) {
                    std::fprintf(stderr, "budyk exec: %s: killed by signal %d (%.2f s)\n",
                                 what.c_str(), res.signal, res.elapsed_seconds);
                } else if (res.exit_status != 0) {
                    std::fprintf(stderr, "budyk exec: %s: exit %d (%.2f s)\n",
                                 what.c_str(), res.exit_status, res.elapsed_seconds);
                }
            });
        out->kind = queued ? ExecOutcome::Queued : ExecOutcome::QueueFull;
        return 0;
    }

    // wait = true: run inline and hand the result back. The tick waits,
    // so the timeout is capped at a minute; a longer job belongs in the
    // default, queued form.
    if (timeout_s > 60) timeout_s = 60;

    std::vector<const char*> argv_ptrs;
    argv_ptrs.reserve(argv_storage.size() + 1);
    for (const auto& s : argv_storage) argv_ptrs.push_back(s.c_str());
    argv_ptrs.push_back(nullptr);

    out->kind = ExecOutcome::Ran;
    out->rc   = budyk::exec_command(argv_ptrs.data(), timeout_s, &out->res);
    return 0;
}

// exec(cmd [, timeout | { timeout, wait }]) — see exec_impl. Written in
// the rules file itself (outside when() and the actions), it runs
// nothing: it checks the command as it would run it and returns a
// function that runs it, so `action = { alert, exec("/path") }` and
// `action = exec("/path")` work as spec §3.6.2 writes them.
int deferred_exec(lua_State* L);

int l_exec(lua_State* L) {
    auto* eng = engine_from(L);
    if (eng == nullptr || !eng->exec_enabled()) {
        return luaL_error(L, "exec is disabled (enable with --enable-exec)");
    }
    char        err[512] = "";
    ExecOutcome out;
    if (exec_impl(L, eng, &out, err, sizeof(err)) != 0) return luaL_error(L, "%s", err);
    switch (out.kind) {
        case ExecOutcome::Checked:      // loading: the command as an action
            lua_settop(L, 2);
            lua_pushcclosure(L, deferred_exec, 2);
            return 1;
        case ExecOutcome::Queued:
        case ExecOutcome::QueueFull: {
            const bool queued = out.kind == ExecOutcome::Queued;
            lua_newtable(L);
            lua_pushboolean(L, queued); lua_setfield(L, -2, "queued");
            lua_pushboolean(L, queued); lua_setfield(L, -2, "ok");
            if (!queued) {
                lua_pushstring(L, "exec queue full"); lua_setfield(L, -2, "error");
            }
            return 1;
        }
        case ExecOutcome::Ran:
            break;
    }
    // The result table regardless of rc: rc < 0 means fork / setup failed
    // before the child could run, surfaced as `error`.
    const budyk::ExecResult& res = out.res;
    lua_newtable(L);
    lua_pushinteger(L, res.exit_status);     lua_setfield(L, -2, "exit_status");
    lua_pushinteger(L, res.signal);          lua_setfield(L, -2, "signal");
    lua_pushboolean(L, res.timed_out);       lua_setfield(L, -2, "timed_out");
    lua_pushnumber (L, res.elapsed_seconds); lua_setfield(L, -2, "elapsed_seconds");
    lua_pushboolean(L, out.rc == 0 && res.exit_status == 0 &&
                       res.signal == 0 && !res.timed_out);
    lua_setfield(L, -2, "ok");
    if (out.rc != 0) {
        lua_pushinteger(L, out.rc); lua_setfield(L, -2, "error");
    }
    return 1;
}

// The action exec() returned while loading: runs exec() with the
// arguments it was given (upvalues 1 and 2) when the rule fires.
int deferred_exec(lua_State* L) {
    lua_settop(L, 0);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, lua_upvalueindex(2));
    return l_exec(L);
}

// Shared implementation of freeze() and unfreeze(). `stop == true` sends
// SIGSTOP; `stop == false` sends SIGCONT. Both share the gate, the
// allowlist match and the return-table shape, so factoring them keeps
// the two bindings honest. Returns one value: a table with
//   { ok = bool, errno = int, pid = int, name = string? }
int freeze_action(lua_State* L, const char* fn_name, bool stop) {
    auto* eng = engine_from(L);
    if (eng == nullptr || !eng->freeze_enabled()) {
        return luaL_error(L,
            "%s is disabled (enable with --enable-freeze)", fn_name);
    }

    const lua_Integer pid_arg = luaL_checkinteger(L, 1);
    if (pid_arg <= 0 || pid_arg > INT_MAX) {
        // The int it was cast to took 2^32 + n as n.
        return luaL_error(L, "%s: pid must be a positive integer", fn_name);
    }
    const int pid = static_cast<int>(pid_arg);
    if (budyk::freeze_refused(pid)) {
        return luaL_error(L, "%s: refusing pid %d (%s)", fn_name, pid,
                          pid == 1 ? "init" : "budyk itself");
    }

    // Resolve the target's `comm` first — needed both for the allowlist
    // check and to surface it back to the rule in the result table.
    char       name_buf[64] = {0};
    const bool name_ok      =
        budyk::proc_name_of(pid, name_buf, sizeof(name_buf)) == 0;

    const auto& allow = eng->freeze_allowlist();
    if (!allow.empty()) {
        if (!name_ok) {
            return luaL_error(L,
                "%s: cannot resolve process name for pid %d", fn_name, pid);
        }
        bool found = false;
        for (const auto& n : allow) {
            if (n == name_buf) { found = true; break; }
        }
        if (!found) {
            return luaL_error(L,
                "%s: process '%s' (pid %d) not in allowlist",
                fn_name, name_buf, pid);
        }
    }

    const int rc = stop ? budyk::freeze_pid(pid) : budyk::unfreeze_pid(pid);

    lua_newtable(L);
    lua_pushboolean(L, rc == 0);          lua_setfield(L, -2, "ok");
    lua_pushinteger(L, rc < 0 ? -rc : 0); lua_setfield(L, -2, "errno");
    lua_pushinteger(L, pid);              lua_setfield(L, -2, "pid");
    if (name_ok) {
        lua_pushstring(L, name_buf);
        lua_setfield(L, -2, "name");
    }
    return 1;
}

int l_freeze  (lua_State* L) { return freeze_action(L, "freeze",   true);  }
int l_unfreeze(lua_State* L) { return freeze_action(L, "unfreeze", false); }

} // namespace

void budyk_lua_register_stdlib(lua_State* L, bool /*enable_exec*/) {
    lua_register(L, "watch",    l_watch);
    lua_register(L, "alert",    l_alert);
    lua_register(L, "escalate", l_escalate);
    lua_register(L, "exec",     l_exec);
    lua_register(L, "freeze",   l_freeze);
    lua_register(L, "unfreeze", l_unfreeze);
}
