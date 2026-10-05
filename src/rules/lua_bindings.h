// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Expose a Sample to Lua as global tables (cpu, mem, swap, load, disk,
// net, proc, entropy, self_, thermal) plus uptime_seconds.
//
// Each global is a read-only proxy over a data table: assigning a field
// raises "cpu.total_percent is read-only", pairs() iterates the real
// fields, getmetatable() returns "read-only". The tables are created on
// the first bind and updated in place afterwards (no allocation per
// tick); every bind re-points the globals and clears anything a rule
// rawset() onto a proxy, so neither outlives the tick.
struct lua_State;
namespace budyk { struct Sample; struct FileWatchState; }
void budyk_lua_bind_sample    (lua_State* L, const budyk::Sample& s);
void budyk_lua_bind_files     (lua_State* L, const budyk::FileWatchState& s);
