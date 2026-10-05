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
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kEngineRegKey = "budyk.engine";

budyk::LuaEngine* engine_from(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kEngineRegKey);
    auto* eng = static_cast<budyk::LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return eng;
}

int opt_int_field(lua_State* L, int tbl, const char* key, int fallback) {
    lua_getfield(L, tbl, key);
    int v = fallback;
    if (lua_isnumber(L, -1)) v = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return v;
}

int l_alert(lua_State* L);   // defined below; watch() recognises it as an action

// watch(name, opts) — registers a rule. opts:
//   when      — required function; the rule fires when it returns true
//   action    — optional:
//                 function  called with no arguments on fire
//                 "alert"   alert(name, severity, message)   (the default)
//                 "log"     print "[budyk] <message>"
//               `action = alert` (the builtin itself) means "alert", so
//               the rule's severity/message are used instead of calling
//               alert() with no arguments.
//   severity  — "info" / "warning" (default) / "critical"; "alert" only
//   message   — string; default is the rule name
//   for_ticks — consecutive true evaluations needed to fire (default 1)
//   cooldown  — ticks to skip after firing (default 0)
// Anything else in action / severity is an error at load time, so a
// misconfigured rule fails loudly instead of silently doing nothing.
int l_watch(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    // Validate every field before creating any registry ref or C++ object:
    // luaL_error longjmps, which would leak a ref and skip destructors.
    lua_getfield(L, 2, "when");
    const bool when_ok = lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (!when_ok) {
        return luaL_error(L, "watch(%s): 'when' must be a function", name);
    }

    enum class Action { Function, Alert, Log, Invalid };
    Action action = Action::Invalid;
    lua_getfield(L, 2, "action");
    switch (lua_type(L, -1)) {
        case LUA_TNIL:
            action = Action::Alert;
            break;
        case LUA_TFUNCTION:
            action = lua_tocfunction(L, -1) == l_alert ? Action::Alert
                                                       : Action::Function;
            break;
        case LUA_TSTRING: {
            const char* tag = lua_tostring(L, -1);
            if      (std::strcmp(tag, "alert") == 0) action = Action::Alert;
            else if (std::strcmp(tag, "log")   == 0) action = Action::Log;
            break;
        }
        default:
            break;
    }
    lua_pop(L, 1);
    if (action == Action::Invalid) {
        return luaL_error(L,
            "watch(%s): 'action' must be a function, \"alert\" or \"log\"", name);
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
    const int msg_type = lua_type(L, -1);
    lua_pop(L, 1);
    if (msg_type != LUA_TNIL && msg_type != LUA_TSTRING) {
        return luaL_error(L, "watch(%s): 'message' must be a string", name);
    }

    auto* eng = engine_from(L);
    if (eng == nullptr) {
        return luaL_error(L, "watch: engine not bound");
    }

    // Nothing below raises.
    const int for_ticks      = opt_int_field(L, 2, "for_ticks", 1);
    const int cooldown_ticks = opt_int_field(L, 2, "cooldown",  0);

    std::string message = name;
    if (msg_type == LUA_TSTRING) {
        lua_getfield(L, 2, "message");
        message = lua_tostring(L, -1);
        lua_pop(L, 1);
    }

    lua_getfield(L, 2, "when");
    const int when_ref   = luaL_ref(L, LUA_REGISTRYINDEX);
    int       action_ref = LUA_REFNIL;
    if (action == Action::Function) {
        lua_getfield(L, 2, "action");
        action_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    const char* action_tag = action == Action::Alert ? "alert"
                           : action == Action::Log   ? "log"
                           : "";

    eng->add_rule(name, when_ref, action_ref, action_tag, severity, message,
                  for_ticks, cooldown_ticks);
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
    const char* msg     = lua_tostring(L, 3);

    // Lenient on purpose: an unknown severity falls back to warning rather
    // than failing the action mid-incident.
    budyk::AlertSeverity sev = budyk::AlertSeverity::Warning;
    budyk::parse_severity(sev_str, &sev);

    const int ok = eng->alerts().dispatch(sev, name, msg ? msg : "");
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

// Does the work of exec(). Returns 1 (result table pushed) or -1 with a
// message in err. It never raises: luaL_error longjmps past C++
// destructors, and this function owns std::string / std::vector objects,
// so l_exec raises only after they are gone. Raw table access keeps
// metamethods from raising in here too.
int exec_impl(lua_State* L, budyk::LuaEngine* eng, char* err, size_t err_cap) {
    // Accept either exec("/path/to/cmd")          (single-arg form)
    //            or exec({"/bin/sh", "-c", "..."}) (argv table form).
    std::vector<std::string> argv_storage;
    if (lua_isstring(L, 1)) {
        argv_storage.emplace_back(lua_tostring(L, 1));
    } else if (lua_istable(L, 1)) {
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
        if (n <= 0) {
            std::snprintf(err, err_cap, "exec: empty argv table");
            return -1;
        }
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, 1, i);
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 1);
                std::snprintf(err, err_cap, "exec: argv[%d] is not a string",
                                  static_cast<int>(i));
                return -1;
            }
            argv_storage.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        std::snprintf(err, err_cap, "exec: expected string or argv table");
        return -1;
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
        // Compare resolved paths (see LuaEngine::exec_allowlist_resolved).
        char rbuf[PATH_MAX];
        const char* r = ::realpath(cmd.c_str(), rbuf);
        const std::string resolved = r != nullptr ? std::string(r) : cmd;
        bool found = false;
        for (const auto& a : allow) if (a == resolved) { found = true; break; }
        if (!found) {
            std::snprintf(err, err_cap, "exec: '%s' not in allowlist", cmd.c_str());
            return -1;
        }
    }

    // Optional second argument: a timeout in seconds, or a table
    //   { timeout = <seconds>, wait = <bool> }.
    // Default 30 s, at most a day; clamped here so a huge Lua integer
    // can't wrap through the int. Raw access: a metamethod must not
    // raise in here (see the function comment).
    int  timeout_s = 30;
    bool wait      = false;
    auto take_timeout = [&](int idx) {
        const lua_Integer t = lua_tointeger(L, idx);
        if (t > 0) timeout_s = static_cast<int>(t < 86400 ? t : 86400);
    };
    if (lua_isnumber(L, 2)) {
        take_timeout(2);
    } else if (lua_istable(L, 2)) {
        lua_pushstring(L, "timeout");
        lua_rawget(L, 2);
        if (lua_isnumber(L, -1)) take_timeout(-1);
        lua_pop(L, 1);
        lua_pushstring(L, "wait");
        lua_rawget(L, 2);
        wait = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }

    if (!wait) {
        // The normal case (spec §3.6: fork+exec, non-blocking). The
        // command runs on the engine's exec worker; the tick does not
        // wait for it, and its outcome goes to the log. Lua gets
        // { queued = true } — or { queued = false, error = "..." } when
        // the queue is full.
        std::string what = cmd;
        for (size_t i = 1; i < argv_storage.size(); ++i) {
            what += ' ';
            what += argv_storage[i];
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
        lua_newtable(L);
        lua_pushboolean(L, queued); lua_setfield(L, -2, "queued");
        lua_pushboolean(L, queued); lua_setfield(L, -2, "ok");
        if (!queued) {
            lua_pushstring(L, "exec queue full"); lua_setfield(L, -2, "error");
        }
        return 1;
    }

    // wait = true: run inline and hand the result back. The tick waits,
    // so the timeout is capped at a minute; a longer job belongs in the
    // default, queued form.
    if (timeout_s > 60) timeout_s = 60;

    std::vector<const char*> argv_ptrs;
    argv_ptrs.reserve(argv_storage.size() + 1);
    for (const auto& s : argv_storage) argv_ptrs.push_back(s.c_str());
    argv_ptrs.push_back(nullptr);

    budyk::ExecResult res{};
    const int rc = budyk::exec_command(argv_ptrs.data(), timeout_s, &res);

    // Push result table regardless of rc — rc < 0 just means fork/setup
    // failed before the child could run; surface that via `error`.
    lua_newtable(L);
    lua_pushinteger(L, res.exit_status);     lua_setfield(L, -2, "exit_status");
    lua_pushinteger(L, res.signal);          lua_setfield(L, -2, "signal");
    lua_pushboolean(L, res.timed_out);       lua_setfield(L, -2, "timed_out");
    lua_pushnumber (L, res.elapsed_seconds); lua_setfield(L, -2, "elapsed_seconds");
    lua_pushboolean(L, rc == 0 && res.exit_status == 0 &&
                       res.signal == 0 && !res.timed_out);
    lua_setfield(L, -2, "ok");
    if (rc != 0) {
        lua_pushinteger(L, rc); lua_setfield(L, -2, "error");
    }
    return 1;
}

int l_exec(lua_State* L) {
    auto* eng = engine_from(L);
    if (eng == nullptr || !eng->exec_enabled()) {
        return luaL_error(L, "exec is disabled (enable with --enable-exec)");
    }
    char err[512] = "";
    const int n = exec_impl(L, eng, err, sizeof(err));
    if (n < 0) return luaL_error(L, "%s", err);
    return n;
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
    if (pid_arg <= 0) {
        return luaL_error(L, "%s: pid must be a positive integer", fn_name);
    }
    const int pid = static_cast<int>(pid_arg);

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
