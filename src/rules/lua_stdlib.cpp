// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_stdlib.h"

#include "rules/exec_action.h"
#include "rules/lua_engine.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

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

int l_watch(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    // opts.when — required, must be a function
    lua_getfield(L, 2, "when");
    if (!lua_isfunction(L, -1)) {
        return luaL_error(L, "watch(%s): 'when' must be a function", name);
    }
    const int when_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // opts.action — optional; function, string ("alert"/"log"), or nil (default alert)
    lua_getfield(L, 2, "action");
    int         action_ref = LUA_REFNIL;
    std::string action_tag;
    if (lua_isfunction(L, -1)) {
        action_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else if (lua_isstring(L, -1)) {
        action_tag = lua_tostring(L, -1);
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        action_tag = "alert";
    }

    // opts.for_ticks + opts.cooldown — optional integers. for_ticks
    // defaults to 1 (fire immediately). cooldown defaults to for_ticks.
    const int for_ticks      = opt_int_field(L, 2, "for_ticks", 1);
    const int cooldown_ticks = opt_int_field(L, 2, "cooldown",  -1);

    auto* eng = engine_from(L);
    if (eng == nullptr) {
        if (when_ref   != LUA_REFNIL) luaL_unref(L, LUA_REGISTRYINDEX, when_ref);
        if (action_ref != LUA_REFNIL) luaL_unref(L, LUA_REGISTRYINDEX, action_ref);
        return luaL_error(L, "watch: engine not bound");
    }

    eng->add_rule(name, when_ref, action_ref, action_tag, for_ticks, cooldown_ticks);
    return 0;
}

int l_alert(lua_State* L) {
    // Placeholder — v1.0 fires `alert` to log + WS event only (spec §7).
    // For the engine MVP this is a no-op sink; users invoke it via
    // opts.action = alert inside their when() or as an explicit call.
    (void)L;
    return 0;
}

int l_escalate(lua_State* L) {
    // Signal to scheduler — wired in a later PR.
    (void)L;
    return 0;
}

int l_exec(lua_State* L) {
    auto* eng = engine_from(L);
    if (eng == nullptr || !eng->exec_enabled()) {
        return luaL_error(L, "exec is disabled (enable with --enable-exec)");
    }

    // Accept either exec("/path/to/cmd")          (single-arg form)
    //            or exec({"/bin/sh", "-c", "..."}) (argv table form).
    std::vector<std::string> argv_storage;
    if (lua_isstring(L, 1)) {
        argv_storage.emplace_back(lua_tostring(L, 1));
    } else if (lua_istable(L, 1)) {
        const lua_Integer n = luaL_len(L, 1);
        if (n <= 0) return luaL_error(L, "exec: empty argv table");
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_geti(L, 1, i);
            if (!lua_isstring(L, -1)) {
                return luaL_error(L, "exec: argv[%d] is not a string",
                                  static_cast<int>(i));
            }
            argv_storage.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        return luaL_error(L, "exec: expected string or argv table");
    }

    std::vector<const char*> argv_ptrs;
    argv_ptrs.reserve(argv_storage.size() + 1);
    for (const auto& s : argv_storage) argv_ptrs.push_back(s.c_str());
    argv_ptrs.push_back(nullptr);

    // Optional second argument: timeout in seconds (default 30).
    int timeout_s = 30;
    if (lua_isnumber(L, 2)) {
        const lua_Integer t = lua_tointeger(L, 2);
        if (t > 0) timeout_s = static_cast<int>(t);
    }

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

} // namespace

void budyk_lua_register_stdlib(lua_State* L, bool /*enable_exec*/) {
    lua_register(L, "watch",    l_watch);
    lua_register(L, "alert",    l_alert);
    lua_register(L, "escalate", l_escalate);
    lua_register(L, "exec",     l_exec);
}
