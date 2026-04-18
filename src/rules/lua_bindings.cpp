// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_bindings.h"

#include "core/sample.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

inline void set_number(lua_State* L, const char* key, double v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, key);
}
inline void set_integer(lua_State* L, const char* key, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}

} // namespace

void budyk_lua_bind_sample(lua_State* L, const budyk::Sample& s) {
    // cpu
    lua_newtable(L);
    set_number (L, "total_percent", s.cpu.total_percent);
    set_integer(L, "count",         static_cast<lua_Integer>(s.cpu.count));
    lua_setglobal(L, "cpu");

    // mem
    lua_newtable(L);
    set_integer(L, "total",             static_cast<lua_Integer>(s.mem.total));
    set_integer(L, "available",         static_cast<lua_Integer>(s.mem.available));
    set_number (L, "available_percent", s.mem.available_percent);
    lua_setglobal(L, "mem");

    // swap
    lua_newtable(L);
    set_integer(L, "total",        static_cast<lua_Integer>(s.swap.total));
    set_integer(L, "used",         static_cast<lua_Integer>(s.swap.used));
    set_number (L, "used_percent", s.swap.used_percent);
    lua_setglobal(L, "swap");

    // load  (shadows the builtin base-library `load` function, which was
    // already nil'd out by the sandbox setup anyway).
    lua_newtable(L);
    set_number(L, "avg_1m",  s.load.avg_1m);
    set_number(L, "avg_5m",  s.load.avg_5m);
    set_number(L, "avg_15m", s.load.avg_15m);
    lua_setglobal(L, "load");

    lua_pushnumber(L, s.uptime_seconds);
    lua_setglobal(L, "uptime_seconds");
}
