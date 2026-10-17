// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Expose a Sample to Lua as global tables (cpu, mem, swap, load, disk,
// net, proc, entropy, self_, thermal, tcp, udp, fs, clock) plus
// uptime_seconds.
//
// Each global is a read-only proxy over a data table: assigning a field
// raises "cpu.total_percent is read-only", pairs() iterates the real
// fields, getmetatable() returns "read-only". The tables are created on
// the first bind and updated in place afterwards (no allocation per
// tick); every bind re-points the globals and clears anything a rule
// rawset() onto a proxy, so neither outlives the tick.
//
// With `d`, the per-entity detail goes in as read-only collections inside
// the groups: cpu.cores (an array), disk.devices, net.interfaces and
// fs.mounts (keyed by name / mount point), those in d->collected. Item
// tables are reused while the item is there and dropped when it goes.
//
// A group the sample lacks (s.groups) and a list `d` lacks (or every list,
// without `d`) has no reading on this tick: reading it, or its length, or
// pairs() over it raises "<name> is not collected at this level", or, for
// a rate read for the first time (s.baseline, d->baseline), "<name> has
// no rate yet: its first read at this level". budyk_lua_uncollected()
// recognises both; the engine skips a rule whose when() raised one (spec
// §3.6.5).
#include "core/sample_c.h"
struct lua_State;
namespace budyk { struct Sample; struct FileWatchState; }
void budyk_lua_bind_sample    (lua_State* L, const budyk::Sample& s,
                               const budyk_detail_c* d = nullptr);
void budyk_lua_bind_files     (lua_State* L, const budyk::FileWatchState& s);
// Whether the value at `idx` is the error the latest read of something
// without a reading raised (or the same text, re-raised): 1 not collected
// at this level, 2 a rate's first read, 0 neither.
int  budyk_lua_uncollected    (lua_State* L, int idx);
// Registers collected(name): whether the sample bound last has that
// group ("disk", "self") or list ("disk.devices"); an unknown name raises.
void budyk_lua_register_collected(lua_State* L);
