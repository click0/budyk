// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_stdlib.h"

#include "rules/lua_engine.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <string>

namespace {

constexpr const char* kEngineRegKey = "budyk.engine";

budyk::LuaEngine* engine_from(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kEngineRegKey);
    auto* eng = static_cast<budyk::LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return eng;
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

    auto* eng = engine_from(L);
    if (eng == nullptr) {
        if (when_ref != LUA_REFNIL)   luaL_unref(L, LUA_REGISTRYINDEX, when_ref);
        if (action_ref != LUA_REFNIL) luaL_unref(L, LUA_REGISTRYINDEX, action_ref);
        return luaL_error(L, "watch: engine not bound");
    }

    eng->add_rule(name, when_ref, action_ref, action_tag);
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
    // Actual fork+exec deferred to a follow-up block (spec §3.6.3 — timeout,
    // setrlimit). The engine merely records that it was called here.
    return 0;
}

} // namespace

void budyk_lua_register_stdlib(lua_State* L, bool /*enable_exec*/) {
    lua_register(L, "watch",    l_watch);
    lua_register(L, "alert",    l_alert);
    lua_register(L, "escalate", l_escalate);
    lua_register(L, "exec",     l_exec);
}
