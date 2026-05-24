// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Push Sample fields into Lua global tables (cpu, mem, net, etc.)
// Read-only — Lua scripts cannot modify Sample.
struct lua_State;
namespace budyk { struct Sample; struct SshAuditStats; struct FileWatchState; }
void budyk_lua_bind_sample    (lua_State* L, const budyk::Sample& s);
void budyk_lua_bind_ssh_audit (lua_State* L, const budyk::SshAuditStats& s);
void budyk_lua_bind_files     (lua_State* L, const budyk::FileWatchState& s);
