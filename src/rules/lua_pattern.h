/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once
/** @file
 * string.find, match, gmatch and gsub for the rules sandbox: Lua 5.4's
 * own (lua_pattern.c, from lstrlib.c), with the matcher's steps charged
 * to the instruction limit. Lua's count hook sees VM instructions only,
 * and one pattern call is a single instruction, so a backtracking
 * pattern used to run unbounded with the collector tick waiting.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lua_State;

/** The library functions, as lua_CFunctions (lstrlib.c's str_find,
 *  str_match, gmatch and str_gsub). */
int budyk_lua_str_find  (struct lua_State* L);
int budyk_lua_str_match (struct lua_State* L);
int budyk_lua_str_gmatch(struct lua_State* L);
int budyk_lua_str_gsub  (struct lua_State* L);

/** Charge `n` steps of work done in C to the instruction limit of the
 *  rules engine that owns `L`; raises (luaL_error) once the limit is
 *  spent. Defined by the engine (lua_engine.cpp). */
void budyk_lua_charge(struct lua_State* L, size_t n);

#ifdef __cplusplus
}
#endif
