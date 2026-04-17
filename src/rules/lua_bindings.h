// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Push Sample fields into Lua global tables (cpu, mem, net, etc.)
// Read-only — Lua scripts cannot modify Sample.
struct lua_State;
namespace budyk { struct Sample; }
void budyk_lua_bind_sample(lua_State* L, const budyk::Sample& s);
