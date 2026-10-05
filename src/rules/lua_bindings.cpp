// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_bindings.h"

#include "core/sample.h"
#include "security/file_watcher.h"

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
inline void set_boolean(lua_State* L, const char* key, bool v) {
    lua_pushboolean(L, v ? 1 : 0);
    lua_setfield(L, -2, key);
}

// registry["budyk.sample"][group] = { [1] = proxy, [2] = data }
constexpr const char* kSampleRegKey = "budyk.sample";

// __newindex of a sample proxy. Sample values are read-only (spec
// §3.6): one rule must not change what the next rule sees. Raises with
// the position of the assignment in the rule.
int sample_ro_newindex(lua_State* L) {
    const char* group = lua_tostring(L, lua_upvalueindex(1));
    const char* key   = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "?";
    luaL_where(L, 1);     // level 1: the Lua function that did the assignment
    lua_pushfstring(L, "%s.%s is read-only (sample values cannot be changed by rules)",
                    group, key);
    lua_concat(L, 2);
    return lua_error(L);
}

// Iterator for __pairs: walks the data table (upvalue 1 of __pairs).
int sample_ro_next(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
    if (lua_next(L, 1) != 0) return 2;
    lua_pushnil(L);
    return 1;
}

// __pairs of a sample proxy: pairs(cpu) iterates the real fields.
int sample_ro_pairs(lua_State* L) {
    lua_pushcfunction(L, sample_ro_next);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushnil(L);
    return 3;
}

// Remove every raw key from the table at `idx`. A proxy is empty by
// construction; a rule can still rawset() a field onto it, which would
// shadow the data from then on. Clearing on every bind bounds such a
// bypass to the rest of the tick, as when the tables were rebuilt.
void clear_raw_keys(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        lua_pop(L, 1);            // value
        lua_pushvalue(L, -1);     // key
        lua_pushnil(L);
        lua_rawset(L, idx);       // clearing an existing field is allowed mid-traversal
    }
}

// Leave the data table of sample group `group` on the stack, creating it
// and its read-only proxy on first use, and (re)point the global `group`
// at the proxy (a rule may have reassigned the global). The data table
// is updated in place on every tick: no table is allocated after the
// first bind (review M6).
void push_group_data(lua_State* L, const char* group) {
    if (lua_getfield(L, LUA_REGISTRYINDEX, kSampleRegKey) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kSampleRegKey);
    }                                                     // reg
    if (lua_getfield(L, -1, group) != LUA_TTABLE) {       // reg, nil
        lua_pop(L, 1);
        lua_createtable(L, 2, 0);                         // reg, entry
        lua_createtable(L, 0, 4);                         // reg, entry, data
        lua_newtable(L);                                  // reg, entry, data, proxy
        lua_createtable(L, 0, 4);                         // reg, entry, data, proxy, mt
        lua_pushvalue(L, -3);
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, group);
        lua_pushcclosure(L, sample_ro_newindex, 1);
        lua_setfield(L, -2, "__newindex");
        lua_pushvalue(L, -3);
        lua_pushcclosure(L, sample_ro_pairs, 1);
        lua_setfield(L, -2, "__pairs");
        lua_pushliteral(L, "read-only");
        lua_setfield(L, -2, "__metatable");               // getmetatable() sees this
        lua_setmetatable(L, -2);                          // reg, entry, data, proxy
        lua_rawseti(L, -3, 1);                            // reg, entry, data
        lua_rawseti(L, -2, 2);                            // reg, entry
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, group);                       // reg, entry
    }
    lua_rawgeti(L, -1, 1);                                // reg, entry, proxy
    clear_raw_keys(L, -1);
    lua_setglobal(L, group);                              // reg, entry
    lua_rawgeti(L, -1, 2);                                // reg, entry, data
    lua_replace(L, -3);                                   // data, entry
    lua_pop(L, 1);                                        // data
}

} // namespace

void budyk_lua_bind_sample(lua_State* L, const budyk::Sample& s) {
    // cpu
    push_group_data(L, "cpu");
    set_number (L, "total_percent", s.cpu.total_percent);
    set_integer(L, "count",         static_cast<lua_Integer>(s.cpu.count));
    lua_pop(L, 1);

    // mem
    push_group_data(L, "mem");
    set_integer(L, "total",             static_cast<lua_Integer>(s.mem.total));
    set_integer(L, "available",         static_cast<lua_Integer>(s.mem.available));
    set_number (L, "available_percent", s.mem.available_percent);
    lua_pop(L, 1);

    // swap
    push_group_data(L, "swap");
    set_integer(L, "total",        static_cast<lua_Integer>(s.swap.total));
    set_integer(L, "used",         static_cast<lua_Integer>(s.swap.used));
    set_number (L, "used_percent", s.swap.used_percent);
    lua_pop(L, 1);

    // load  (shadows the builtin base-library `load` function, which was
    // already nil'd out by the sandbox setup anyway).
    push_group_data(L, "load");
    set_number(L, "avg_1m",  s.load.avg_1m);
    set_number(L, "avg_5m",  s.load.avg_5m);
    set_number(L, "avg_15m", s.load.avg_15m);
    lua_pop(L, 1);

    // disk — aggregate throughput across whole block devices.
    push_group_data(L, "disk");
    set_integer(L, "read_bytes_per_sec",  static_cast<lua_Integer>(s.disk.read_bytes_per_sec));
    set_integer(L, "write_bytes_per_sec", static_cast<lua_Integer>(s.disk.write_bytes_per_sec));
    set_integer(L, "device_count",        static_cast<lua_Integer>(s.disk.device_count));
    lua_pop(L, 1);

    // net — aggregate throughput across non-loopback interfaces.
    push_group_data(L, "net");
    set_integer(L, "rx_bytes_per_sec",   static_cast<lua_Integer>(s.net.rx_bytes_per_sec));
    set_integer(L, "tx_bytes_per_sec",   static_cast<lua_Integer>(s.net.tx_bytes_per_sec));
    set_integer(L, "interface_count",    static_cast<lua_Integer>(s.net.interface_count));
    lua_pop(L, 1);

    // proc — running / total process counts.
    push_group_data(L, "proc");
    set_integer(L, "total",   static_cast<lua_Integer>(s.proc.total));
    set_integer(L, "running", static_cast<lua_Integer>(s.proc.running));
    lua_pop(L, 1);

    // entropy — kernel CSPRNG pool depth in bits (Linux only).
    push_group_data(L, "entropy");
    set_integer(L, "available_bits", static_cast<lua_Integer>(s.entropy.available_bits));
    set_boolean(L, "present",        s.entropy.present);
    lua_pop(L, 1);

    // self — daemon's own RSS / peak / CPU consumption.
    push_group_data(L, "self_");
    set_integer(L, "rss_bytes",          static_cast<lua_Integer>(s.self_.rss_bytes));
    set_integer(L, "peak_rss_bytes",     static_cast<lua_Integer>(s.self_.peak_rss_bytes));
    set_number (L, "cpu_user_seconds",   s.self_.cpu_user_seconds);
    set_number (L, "cpu_system_seconds", s.self_.cpu_system_seconds);
    lua_pop(L, 1);

    // thermal — hottest sensor reading across thermal_zone* / cpu.<N>.
    push_group_data(L, "thermal");
    set_number (L, "max_celsius",  s.thermal.max_celsius);
    set_integer(L, "sensor_count", static_cast<lua_Integer>(s.thermal.sensor_count));
    set_boolean(L, "present",      s.thermal.present);
    lua_pop(L, 1);

    lua_pushnumber(L, s.uptime_seconds);
    lua_setglobal(L, "uptime_seconds");
}

void budyk_lua_bind_files(lua_State* L, const budyk::FileWatchState& s) {
    // `files` is a path-keyed table; each entry holds:
    //   modifies  — cumulative count of Modified / Created events
    //   deletes   — cumulative count of Deleted events
    //   tampered  — true iff the path fired any event in the most
    //               recent poll cycle (tick-scoped)
    //
    // Path coverage is the union of every path the watcher has ever
    // seen an event for. A watched-but-never-modified path is absent
    // until its first event — rules should guard with `if files[p]`.
    lua_newtable(L);
    auto bump_modify = [&](const std::string& path) {
        // Find or create files[path]
        lua_pushlstring(L, path.data(), path.size());
        lua_pushvalue(L, -1);
        lua_gettable(L, -3);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            set_integer(L, "modifies", 0);
            set_integer(L, "deletes",  0);
            lua_pushboolean(L, 0);
            lua_setfield(L, -2, "tampered");
        }
        // Sub-table now on top, path-key just below.
        return; // caller continues editing the sub-table
    };
    (void)bump_modify;   // unused — kept for clarity; we walk inline.

    // Inline path-walk: union over modifies + deletes + tampered.
    std::unordered_map<std::string, int> seen;   // path → table-index (1-based)
    int                                  count = 0;
    auto ensure_entry = [&](const std::string& path) {
        auto it = seen.find(path);
        if (it != seen.end()) return it->second;
        ++count;
        lua_newtable(L);
        set_integer(L, "modifies", 0);
        set_integer(L, "deletes",  0);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "tampered");
        lua_setfield(L, -2, path.c_str());
        seen.emplace(path, count);
        return count;
    };

    for (const auto& kv : s.modifies) {
        ensure_entry(kv.first);
        lua_getfield(L, -1, kv.first.c_str());
        set_integer(L, "modifies", static_cast<lua_Integer>(kv.second));
        lua_pop(L, 1);
    }
    for (const auto& kv : s.deletes) {
        ensure_entry(kv.first);
        lua_getfield(L, -1, kv.first.c_str());
        set_integer(L, "deletes", static_cast<lua_Integer>(kv.second));
        lua_pop(L, 1);
    }
    for (const auto& p : s.tampered_this_tick) {
        ensure_entry(p);
        lua_getfield(L, -1, p.c_str());
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "tampered");
        lua_pop(L, 1);
    }

    lua_setglobal(L, "files");
}
