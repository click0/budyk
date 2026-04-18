// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_engine.h"

#include "rules/lua_bindings.h"
#include "rules/lua_stdlib.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace budyk {

namespace {

constexpr const char* kEngineRegKey = "budyk.engine";

void open_sandbox_libs(lua_State* L) {
    // Only the safe subset: base + math + string + table.
    luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);

    // Remove unsafe globals that `luaopen_base` also registered.
    const char* banned[] = { "dofile", "loadfile", "load", "loadstring", "require" };
    for (const char* name : banned) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
}

} // namespace

int LuaEngine::init(bool enable_exec) {
    if (L_ != nullptr) return -1;
    exec_enabled_ = enable_exec;

    lua_State* L = luaL_newstate();
    if (L == nullptr) return -2;

    open_sandbox_libs(L);

    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, kEngineRegKey);

    budyk_lua_register_stdlib(L, enable_exec);

    L_ = L;
    return 0;
}

void LuaEngine::shutdown() {
    if (L_ == nullptr) return;
    for (const auto& r : rules_) {
        luaL_unref(L_, LUA_REGISTRYINDEX, r.when_ref);
        if (r.action_ref != LUA_REFNIL) {
            luaL_unref(L_, LUA_REGISTRYINDEX, r.action_ref);
        }
    }
    rules_.clear();
    lua_close(L_);
    L_ = nullptr;
    last_fire_count_ = 0;
}

int LuaEngine::load_string(const char* code) {
    if (L_ == nullptr || code == nullptr) return -1;
    if (luaL_dostring(L_, code) != LUA_OK) {
        lua_pop(L_, 1); // error message
        return -2;
    }
    return 0;
}

int LuaEngine::load_file(const char* path) {
    if (L_ == nullptr || path == nullptr) return -1;
    if (luaL_dofile(L_, path) != LUA_OK) {
        lua_pop(L_, 1);
        return -2;
    }
    return 0;
}

int LuaEngine::eval_tick(const Sample& s) {
    if (L_ == nullptr) return -1;

    budyk_lua_bind_sample(L_, s);
    last_fire_count_ = 0;

    for (auto& r : rules_) {
        if (r.cooldown_remaining > 0) {
            --r.cooldown_remaining;
            continue;
        }

        lua_rawgeti(L_, LUA_REGISTRYINDEX, r.when_ref);
        if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
            lua_pop(L_, 1);
            r.consecutive_hits = 0;
            continue;
        }
        const bool hit = lua_toboolean(L_, -1) != 0;
        lua_pop(L_, 1);

        if (!hit) {
            r.consecutive_hits = 0;
            continue;
        }

        ++r.consecutive_hits;
        if (r.consecutive_hits < r.for_ticks) continue;

        ++last_fire_count_;
        ++r.fire_count;
        r.consecutive_hits   = 0;
        r.cooldown_remaining = r.cooldown_ticks;

        if (r.action_ref != LUA_REFNIL) {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, r.action_ref);
            if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
                lua_pop(L_, 1);
            }
        }
    }
    return last_fire_count_;
}

int  LuaEngine::rule_count()      const { return static_cast<int>(rules_.size()); }
int  LuaEngine::last_fire_count() const { return last_fire_count_; }
bool LuaEngine::exec_enabled()    const { return exec_enabled_; }

const std::vector<LuaRule>& LuaEngine::rules() const { return rules_; }

void LuaEngine::add_rule(const std::string& name, int when_ref, int action_ref,
                         const std::string& action_tag,
                         int for_ticks, int cooldown_ticks) {
    if (for_ticks      < 1) for_ticks      = 1;
    if (cooldown_ticks < 0) cooldown_ticks = 0;   // default: no cooldown
    rules_.push_back(LuaRule{
        name, when_ref, action_ref, action_tag,
        /*fire_count*/ 0,
        for_ticks, cooldown_ticks,
        /*consecutive_hits*/ 0, /*cooldown_remaining*/ 0
    });
}

} // namespace budyk
