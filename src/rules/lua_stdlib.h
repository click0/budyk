// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Register budyk builtins into Lua: watch(), alert(), exec(), escalate(), log()
struct lua_State;
void budyk_lua_register_stdlib(lua_State* L, bool enable_exec);
