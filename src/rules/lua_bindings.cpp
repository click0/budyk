// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_bindings.h"

#include "core/metric_set.h"
#include "core/sample.h"
#include "security/file_watcher.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstring>
#include <utility>

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

// __len of a proxy: #cpu.cores is the length of the data behind it.
int sample_ro_len(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(lua_rawlen(L, lua_upvalueindex(1))));
    return 1;
}

// Push a read-only proxy over the table at `data_idx`; `name` goes into
// the error an assignment raises ("<name>.<key> is read-only").
void push_ro_proxy(lua_State* L, int data_idx, const char* name) {
    data_idx = lua_absindex(L, data_idx);
    lua_newtable(L);                                      // proxy
    lua_createtable(L, 0, 5);                             // proxy, mt
    lua_pushvalue(L, data_idx);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, name);
    lua_pushcclosure(L, sample_ro_newindex, 1);
    lua_setfield(L, -2, "__newindex");
    lua_pushvalue(L, data_idx);
    lua_pushcclosure(L, sample_ro_pairs, 1);
    lua_setfield(L, -2, "__pairs");
    lua_pushvalue(L, data_idx);
    lua_pushcclosure(L, sample_ro_len, 1);
    lua_setfield(L, -2, "__len");
    lua_pushliteral(L, "read-only");
    lua_setfield(L, -2, "__metatable");                   // getmetatable() sees this
    lua_setmetatable(L, -2);                              // proxy
}

// The registry keys of the error that a read of something not collected
// (uncollected()) or not yet rated (baseline()) raised last; see
// budyk_lua_uncollected().
constexpr const char* kUncollectedKey = "budyk.uncollected";
constexpr const char* kBaselineKey    = "budyk.baseline";

// Raise "<position>: <upvalue 1><what>" from a proxy handler and keep the
// error under `key`, so the engine can tell it apart. The position is
// the rule's line: the nearest Lua frame (pairs() or # put a C function
// in between).
int raise_unread(lua_State* L, const char* what, const char* key) {
    lua_Debug ar;
    int level = 1;
    while (lua_getstack(L, level, &ar) != 0 && lua_getinfo(L, "Sl", &ar) != 0 &&
           ar.currentline <= 0) {
        ++level;
    }
    luaL_where(L, level);
    lua_pushfstring(L, "%s%s", lua_tostring(L, lua_upvalueindex(1)), what);
    lua_concat(L, 2);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, key);
    return lua_error(L);
}

// __index, __pairs and __len of a proxy over a group or list the sample
// does not have (upvalue 1: its name). Whatever the rule reads of it, the
// answer would be a zero, not a reading: raise instead.
int uncollected(lua_State* L) {   // the level does not read it
    return raise_unread(L, " is not collected at this level", kUncollectedKey);
}
int baseline(lua_State* L) {      // read for the first time in a row
    return raise_unread(L, " has no rate yet: its first read at this level", kBaselineKey);
}

constexpr const char* kHandlerKeys[] = { "__index", "__pairs", "__len" };

// What a proxy reads: its data, or one of the two errors above.
enum ProxyState : lua_Integer { kReadable = 0, kUncollected = 1, kBaseline = 2 };

// For the registry entry at `entry_idx`, whose [1] is a fresh proxy: keep
// the proxy's metatable in [4] and its handlers for each ProxyState in
// [5 + state]; [8] = the current state (kReadable).
void add_handlers(lua_State* L, int entry_idx, const char* name) {
    entry_idx = lua_absindex(L, entry_idx);
    lua_rawgeti(L, entry_idx, 1);                         // proxy
    lua_getmetatable(L, -1);                              // proxy, mt
    lua_createtable(L, 0, 3);                             // proxy, mt, readable
    for (const char* k : kHandlerKeys) {
        lua_getfield(L, -2, k);
        lua_setfield(L, -2, k);
    }
    lua_rawseti(L, entry_idx, 5 + kReadable);             // proxy, mt
    for (const auto& h : { std::make_pair(kUncollected, &uncollected),
                           std::make_pair(kBaseline,    &baseline) }) {
        lua_createtable(L, 0, 3);                         // proxy, mt, handlers
        lua_pushstring(L, name);
        lua_pushcclosure(L, h.second, 1);
        for (const char* k : kHandlerKeys) {
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, k);
        }
        lua_pop(L, 1);
        lua_rawseti(L, entry_idx, 5 + static_cast<int>(h.first));
    }
    lua_rawseti(L, entry_idx, 4);                         // proxy
    lua_pop(L, 1);
    lua_pushinteger(L, kReadable);
    lua_rawseti(L, entry_idx, 8);
}

// Point the proxy of the entry at `entry_idx` at its data or at the
// handlers of `state`. Only a change touches the metatable.
void set_state(lua_State* L, int entry_idx, ProxyState state) {
    entry_idx = lua_absindex(L, entry_idx);
    lua_rawgeti(L, entry_idx, 8);
    const lua_Integer now = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (now == state) return;
    lua_rawgeti(L, entry_idx, 4);                         // mt
    lua_rawgeti(L, entry_idx, 5 + static_cast<int>(state));   // mt, handlers
    for (const char* k : kHandlerKeys) {
        lua_getfield(L, -1, k);
        lua_setfield(L, -3, k);
    }
    lua_pop(L, 2);
    lua_pushinteger(L, state);
    lua_rawseti(L, entry_idx, 8);
}

// The state of group / list `bit` given what the tick has and what it
// read for the first time.
ProxyState state_of(uint32_t have, uint32_t first_read, uint32_t bit) {
    if ((have & bit) != 0)       return kReadable;
    if ((first_read & bit) != 0) return kBaseline;
    return kUncollected;
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
// first bind (review M6). Unless kReadable, the proxy raises on reads.
void push_group_data(lua_State* L, const char* group, ProxyState state) {
    if (lua_getfield(L, LUA_REGISTRYINDEX, kSampleRegKey) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kSampleRegKey);
    }                                                     // reg
    if (lua_getfield(L, -1, group) != LUA_TTABLE) {       // reg, nil
        lua_pop(L, 1);
        lua_createtable(L, 8, 0);                         // reg, entry
        lua_createtable(L, 0, 4);                         // reg, entry, data
        push_ro_proxy(L, -1, group);                      // reg, entry, data, proxy
        lua_rawseti(L, -3, 1);                            // reg, entry, data
        lua_rawseti(L, -2, 2);                            // reg, entry
        add_handlers(L, -1, group);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, group);                       // reg, entry
    }
    set_state(L, -1, state);
    lua_rawgeti(L, -1, 1);                                // reg, entry, proxy
    clear_raw_keys(L, -1);
    lua_setglobal(L, group);                              // reg, entry
    lua_rawgeti(L, -1, 2);                                // reg, entry, data
    lua_replace(L, -3);                                   // data, entry
    lua_pop(L, 1);                                        // data
}

// registry["budyk.detail"][path] = { [1] = proxy, [2] = data, [3] = items }
// for the collections (cpu.cores, disk.devices, ...); items[key] =
// { [1] = proxy, [2] = data } for every item seen and still present.
constexpr const char* kDetailRegKey = "budyk.detail";

// Leave on the stack `items, data` of collection `path`, the data emptied
// for this tick, creating them and the collection's read-only proxy on
// first use; the group data table at `group_idx` gets [key] = proxy.
// Unless kReadable, the proxy raises on reads.
void push_collection(lua_State* L, int group_idx, const char* path, const char* key,
                     ProxyState state) {
    group_idx = lua_absindex(L, group_idx);
    if (lua_getfield(L, LUA_REGISTRYINDEX, kDetailRegKey) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kDetailRegKey);
    }                                                     // reg
    if (lua_getfield(L, -1, path) != LUA_TTABLE) {        // reg, nil
        lua_pop(L, 1);
        lua_createtable(L, 8, 0);                         // reg, entry
        lua_newtable(L);                                  // reg, entry, data
        push_ro_proxy(L, -1, path);                       // reg, entry, data, proxy
        lua_rawseti(L, -3, 1);                            // reg, entry, data
        lua_rawseti(L, -2, 2);                            // reg, entry
        lua_newtable(L);
        lua_rawseti(L, -2, 3);                            // entry[3] = items
        add_handlers(L, -1, path);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, path);                        // reg, entry
    }
    set_state(L, -1, state);
    lua_rawgeti(L, -1, 1);                                // reg, entry, proxy
    clear_raw_keys(L, -1);
    lua_setfield(L, group_idx, key);                      // reg, entry
    lua_rawgeti(L, -1, 3);                                // reg, entry, items
    lua_rawgeti(L, -2, 2);                                // reg, entry, items, data
    clear_raw_keys(L, -1);
    lua_replace(L, -3);                                   // reg, data, items
    lua_replace(L, -3);                                   // items, data
}

// With the item's key on top of the stack: pop it, set data[key] to the
// item's read-only proxy and leave the item's data table on the stack,
// both created on the item's first appearance.
void push_item(lua_State* L, int items_idx, int data_idx, const char* path) {
    lua_pushvalue(L, -1);                                 // key, key
    if (lua_rawget(L, items_idx) != LUA_TTABLE) {         // key, nil
        lua_pop(L, 1);                                    // key
        lua_createtable(L, 2, 0);                         // key, entry
        lua_newtable(L);                                  // key, entry, idata
        if (lua_type(L, -3) == LUA_TNUMBER) {
            lua_pushfstring(L, "%s[%d]", path, static_cast<int>(lua_tointeger(L, -3)));
        } else {
            lua_pushfstring(L, "%s[\"%s\"]", path, lua_tostring(L, -3));
        }                                                 // key, entry, idata, name
        push_ro_proxy(L, -2, lua_tostring(L, -1));        // key, entry, idata, name, iproxy
        lua_remove(L, -2);                                // key, entry, idata, iproxy
        lua_rawseti(L, -3, 1);                            // key, entry, idata
        lua_rawseti(L, -2, 2);                            // key, entry
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_rawset(L, items_idx);                         // items[key] = entry
    }                                                     // key, entry
    lua_rawgeti(L, -1, 1);                                // key, entry, iproxy
    clear_raw_keys(L, -1);
    lua_pushvalue(L, -3);                                 // key, entry, iproxy, key
    lua_insert(L, -2);                                    // key, entry, key, iproxy
    lua_rawset(L, data_idx);                              // key, entry
    lua_rawgeti(L, -1, 2);                                // key, entry, idata
    lua_replace(L, -3);                                   // idata, entry
    lua_pop(L, 1);                                        // idata
}

// Forget the items that are no longer in the collection (a disk that
// went away, a veth that came and went), so the cache does not grow.
void drop_stale_items(lua_State* L, int items_idx, int data_idx) {
    lua_pushnil(L);
    while (lua_next(L, items_idx) != 0) {                 // key, entry
        lua_pop(L, 1);                                    // key
        lua_pushvalue(L, -1);
        if (lua_rawget(L, data_idx) == LUA_TNIL) {        // key, nil
            lua_pop(L, 1);
            lua_pushvalue(L, -1);
            lua_pushnil(L);
            lua_rawset(L, items_idx);                     // clearing during traversal is allowed
        } else {
            lua_pop(L, 1);
        }
    }
}

// The state of list `bit` in `d` (none: not collected).
ProxyState has(const budyk_detail_c* d, uint32_t bit) {
    return d != nullptr ? state_of(d->collected, d->baseline, bit) : kUncollected;
}

void bind_cores(lua_State* L, int cpu_idx, const budyk_detail_c* d) {
    push_collection(L, cpu_idx, "cpu.cores", "cores", has(d, budyk::kDetailCores));
    const int items = lua_absindex(L, -2), data = lua_absindex(L, -1);
    for (uint32_t i = 0; d != nullptr && i < d->ncores; ++i) {
        const budyk_core_c& c = d->cores[i];
        lua_pushinteger(L, static_cast<lua_Integer>(i) + 1);
        push_item(L, items, data, "cpu.cores");
        set_number(L, "total_percent",  c.total_percent);
        set_number(L, "user_percent",   c.user_percent);
        set_number(L, "system_percent", c.system_percent);
        set_number(L, "iowait_percent", c.iowait_percent);
        set_number(L, "irq_percent",    c.irq_percent);
        set_number(L, "steal_percent",  c.steal_percent);
        set_number(L, "idle_percent",   c.idle_percent);
        lua_pop(L, 1);
    }
    drop_stale_items(L, items, data);
    lua_pop(L, 2);
}

void bind_disks(lua_State* L, int disk_idx, const budyk_detail_c* d) {
    push_collection(L, disk_idx, "disk.devices", "devices", has(d, budyk::kDetailDisks));
    const int items = lua_absindex(L, -2), data = lua_absindex(L, -1);
    for (uint32_t i = 0; d != nullptr && i < d->ndisks; ++i) {
        const budyk_disk_dev_c& x = d->disks[i];
        lua_pushstring(L, x.name);
        push_item(L, items, data, "disk.devices");
        set_integer(L, "read_bytes_per_sec",  static_cast<lua_Integer>(x.read_bytes_per_sec));
        set_integer(L, "write_bytes_per_sec", static_cast<lua_Integer>(x.write_bytes_per_sec));
        set_number (L, "reads_per_sec",       x.reads_per_sec);
        set_number (L, "writes_per_sec",      x.writes_per_sec);
        set_number (L, "busy_percent",        x.busy_percent);
        lua_pop(L, 1);
    }
    drop_stale_items(L, items, data);
    lua_pop(L, 2);
}

void bind_interfaces(lua_State* L, int net_idx, const budyk_detail_c* d) {
    push_collection(L, net_idx, "net.interfaces", "interfaces", has(d, budyk::kDetailInterfaces));
    const int items = lua_absindex(L, -2), data = lua_absindex(L, -1);
    for (uint32_t i = 0; d != nullptr && i < d->nifaces; ++i) {
        const budyk_iface_c& x = d->ifaces[i];
        lua_pushstring(L, x.name);
        push_item(L, items, data, "net.interfaces");
        set_integer(L, "rx_bytes_per_sec",   static_cast<lua_Integer>(x.rx_bytes_per_sec));
        set_integer(L, "tx_bytes_per_sec",   static_cast<lua_Integer>(x.tx_bytes_per_sec));
        set_number (L, "rx_packets_per_sec", x.rx_packets_per_sec);
        set_number (L, "tx_packets_per_sec", x.tx_packets_per_sec);
        set_number (L, "rx_errors_per_sec",  x.rx_errors_per_sec);
        set_number (L, "tx_errors_per_sec",  x.tx_errors_per_sec);
        set_number (L, "rx_drops_per_sec",   x.rx_drops_per_sec);
        set_boolean(L, "up",                 x.up != 0);
        lua_pop(L, 1);
    }
    drop_stale_items(L, items, data);
    lua_pop(L, 2);
}

void bind_mounts(lua_State* L, int fs_idx, const budyk_detail_c* d) {
    push_collection(L, fs_idx, "fs.mounts", "mounts", has(d, budyk::kDetailMounts));
    const int items = lua_absindex(L, -2), data = lua_absindex(L, -1);
    for (uint32_t i = 0; d != nullptr && i < d->nmounts; ++i) {
        const budyk_mount_c& x = d->mounts[i];
        lua_pushstring(L, x.mount);
        push_item(L, items, data, "fs.mounts");
        lua_pushstring(L, x.type);
        lua_setfield(L, -2, "type");
        lua_pushstring(L, x.device);
        lua_setfield(L, -2, "device");
        set_integer(L, "total_bytes",         static_cast<lua_Integer>(x.total_bytes));
        set_integer(L, "used_bytes",          static_cast<lua_Integer>(x.used_bytes));
        set_integer(L, "avail_bytes",         static_cast<lua_Integer>(x.avail_bytes));
        set_number (L, "used_percent",        x.used_percent);
        set_integer(L, "inodes_total",        static_cast<lua_Integer>(x.inodes_total));
        set_integer(L, "inodes_used",         static_cast<lua_Integer>(x.inodes_used));
        set_number (L, "inodes_used_percent", x.inodes_used_percent);
        lua_pop(L, 1);
    }
    drop_stale_items(L, items, data);
    lua_pop(L, 2);
}

void bind_processes(lua_State* L, int proc_idx, const budyk_detail_c* d) {
    push_collection(L, proc_idx, "proc.top", "top", has(d, budyk::kDetailProcesses));
    const int items = lua_absindex(L, -2), data = lua_absindex(L, -1);
    for (uint32_t i = 0; d != nullptr && i < d->nprocs; ++i) {
        const budyk_process_c& p = d->procs[i];
        lua_pushinteger(L, static_cast<lua_Integer>(i) + 1);
        push_item(L, items, data, "proc.top");
        set_integer(L, "pid", p.pid);
        lua_pushstring(L, p.name);
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, &p.state, p.state != '\0' ? 1 : 0);
        lua_setfield(L, -2, "state");
        set_integer(L, "threads",   static_cast<lua_Integer>(p.threads));
        set_number (L, "cpu_percent", p.cpu_percent);
        set_integer(L, "rss_bytes", static_cast<lua_Integer>(p.rss_bytes));
        lua_pop(L, 1);
    }
    drop_stale_items(L, items, data);
    lua_pop(L, 2);
}

} // namespace

void budyk_lua_bind_sample(lua_State* L, const budyk::Sample& s, const budyk_detail_c* d) {
    // For collected().
    lua_pushinteger(L, static_cast<lua_Integer>(s.groups));
    lua_setfield(L, LUA_REGISTRYINDEX, "budyk.groups");
    lua_pushinteger(L, static_cast<lua_Integer>(d != nullptr ? d->collected : 0));
    lua_setfield(L, LUA_REGISTRYINDEX, "budyk.lists");

    // cpu
    push_group_data(L, "cpu", state_of(s.groups, s.baseline, budyk::kGroupCpu));
    set_number (L, "total_percent",  s.cpu.total_percent);
    set_integer(L, "count",          static_cast<lua_Integer>(s.cpu.count));
    set_number (L, "user_percent",   s.cpu.user_percent);
    set_number (L, "system_percent", s.cpu.system_percent);
    set_number (L, "iowait_percent", s.cpu.iowait_percent);
    set_number (L, "irq_percent",    s.cpu.irq_percent);
    set_number (L, "steal_percent",  s.cpu.steal_percent);
    set_number (L, "idle_percent",   s.cpu.idle_percent);
    bind_cores(L, -1, d);
    lua_pop(L, 1);

    // mem
    push_group_data(L, "mem", state_of(s.groups, s.baseline, budyk::kGroupMem));
    set_integer(L, "total",             static_cast<lua_Integer>(s.mem.total));
    set_integer(L, "available",         static_cast<lua_Integer>(s.mem.available));
    set_number (L, "available_percent", s.mem.available_percent);
    lua_pop(L, 1);

    // swap
    push_group_data(L, "swap", state_of(s.groups, s.baseline, budyk::kGroupSwap));
    set_integer(L, "total",        static_cast<lua_Integer>(s.swap.total));
    set_integer(L, "used",         static_cast<lua_Integer>(s.swap.used));
    set_number (L, "used_percent", s.swap.used_percent);
    lua_pop(L, 1);

    // load  (shadows the builtin base-library `load` function, which was
    // already nil'd out by the sandbox setup anyway).
    push_group_data(L, "load", state_of(s.groups, s.baseline, budyk::kGroupLoad));
    set_number(L, "avg_1m",  s.load.avg_1m);
    set_number(L, "avg_5m",  s.load.avg_5m);
    set_number(L, "avg_15m", s.load.avg_15m);
    lua_pop(L, 1);

    // disk — aggregate throughput across whole block devices.
    push_group_data(L, "disk", state_of(s.groups, s.baseline, budyk::kGroupDisk));
    set_integer(L, "read_bytes_per_sec",  static_cast<lua_Integer>(s.disk.read_bytes_per_sec));
    set_integer(L, "write_bytes_per_sec", static_cast<lua_Integer>(s.disk.write_bytes_per_sec));
    set_integer(L, "device_count",        static_cast<lua_Integer>(s.disk.device_count));
    bind_disks(L, -1, d);
    lua_pop(L, 1);

    // net — aggregate throughput across non-loopback interfaces.
    push_group_data(L, "net", state_of(s.groups, s.baseline, budyk::kGroupNet));
    set_integer(L, "rx_bytes_per_sec",   static_cast<lua_Integer>(s.net.rx_bytes_per_sec));
    set_integer(L, "tx_bytes_per_sec",   static_cast<lua_Integer>(s.net.tx_bytes_per_sec));
    set_integer(L, "interface_count",    static_cast<lua_Integer>(s.net.interface_count));
    bind_interfaces(L, -1, d);
    lua_pop(L, 1);

    // proc — processes by state, and threads.
    push_group_data(L, "proc", state_of(s.groups, s.baseline, budyk::kGroupProc));
    set_integer(L, "total",    static_cast<lua_Integer>(s.proc.total));
    set_integer(L, "running",  static_cast<lua_Integer>(s.proc.running));
    set_integer(L, "sleeping", static_cast<lua_Integer>(s.proc.sleeping));
    set_integer(L, "blocked",  static_cast<lua_Integer>(s.proc.blocked));
    set_integer(L, "stopped",  static_cast<lua_Integer>(s.proc.stopped));
    set_integer(L, "zombie",   static_cast<lua_Integer>(s.proc.zombie));
    set_integer(L, "threads",  static_cast<lua_Integer>(s.proc.threads));
    bind_processes(L, -1, d);
    lua_pop(L, 1);

    // entropy — kernel CSPRNG pool depth in bits (Linux only).
    push_group_data(L, "entropy", state_of(s.groups, s.baseline, budyk::kGroupEntropy));
    set_integer(L, "available_bits", static_cast<lua_Integer>(s.entropy.available_bits));
    set_boolean(L, "present",        s.entropy.present);
    lua_pop(L, 1);

    // self — daemon's own RSS / peak / CPU consumption.
    push_group_data(L, "self_", state_of(s.groups, s.baseline, budyk::kGroupSelf));
    set_integer(L, "rss_bytes",          static_cast<lua_Integer>(s.self_.rss_bytes));
    set_integer(L, "peak_rss_bytes",     static_cast<lua_Integer>(s.self_.peak_rss_bytes));
    set_number (L, "cpu_user_seconds",   s.self_.cpu_user_seconds);
    set_number (L, "cpu_system_seconds", s.self_.cpu_system_seconds);
    lua_pop(L, 1);

    // thermal — hottest sensor reading across thermal_zone* / cpu.<N>.
    push_group_data(L, "thermal", state_of(s.groups, s.baseline, budyk::kGroupThermal));
    set_number (L, "max_celsius",  s.thermal.max_celsius);
    set_integer(L, "sensor_count", static_cast<lua_Integer>(s.thermal.sensor_count));
    set_boolean(L, "present",      s.thermal.present);
    lua_pop(L, 1);

    // tcp / udp — sockets by state and per-second rates.
    push_group_data(L, "tcp", state_of(s.groups, s.baseline, budyk::kGroupTcp));
    set_integer(L, "established",           static_cast<lua_Integer>(s.tcp.established));
    set_integer(L, "time_wait",             static_cast<lua_Integer>(s.tcp.time_wait));
    set_number (L, "active_opens_per_sec",  s.tcp.active_opens_per_sec);
    set_number (L, "passive_opens_per_sec", s.tcp.passive_opens_per_sec);
    set_number (L, "retransmits_per_sec",   s.tcp.retransmits_per_sec);
    set_number (L, "in_segments_per_sec",   s.tcp.in_segments_per_sec);
    set_number (L, "out_segments_per_sec",  s.tcp.out_segments_per_sec);
    set_number (L, "in_errors_per_sec",     s.tcp.in_errors_per_sec);
    set_boolean(L, "present",               s.tcp.present);
    lua_pop(L, 1);

    push_group_data(L, "udp", state_of(s.groups, s.baseline, budyk::kGroupUdp));
    set_number (L, "in_datagrams_per_sec",  s.udp.in_datagrams_per_sec);
    set_number (L, "out_datagrams_per_sec", s.udp.out_datagrams_per_sec);
    set_number (L, "no_port_per_sec",       s.udp.no_port_per_sec);
    set_number (L, "in_errors_per_sec",     s.udp.in_errors_per_sec);
    set_boolean(L, "present",               s.udp.present);
    lua_pop(L, 1);

    push_group_data(L, "icmp", state_of(s.groups, s.baseline, budyk::kGroupIcmp));
    set_number (L, "in_msgs_per_sec",          s.icmp.in_msgs_per_sec);
    set_number (L, "out_msgs_per_sec",         s.icmp.out_msgs_per_sec);
    set_number (L, "in_errors_per_sec",        s.icmp.in_errors_per_sec);
    set_number (L, "in_dest_unreachs_per_sec", s.icmp.in_dest_unreachs_per_sec);
    set_boolean(L, "present",                  s.icmp.present);
    lua_pop(L, 1);

    // fs — local filesystems: how many, the fullest by space and inodes.
    push_group_data(L, "fs", state_of(s.groups, s.baseline, budyk::kGroupFs));
    set_integer(L, "count",                   static_cast<lua_Integer>(s.fs.count));
    set_number (L, "max_used_percent",        s.fs.max_used_percent);
    set_number (L, "max_inodes_used_percent", s.fs.max_inodes_used_percent);
    set_boolean(L, "present",                 s.fs.present);
    bind_mounts(L, -1, d);
    lua_pop(L, 1);

    // clock — the kernel's NTP discipline.
    push_group_data(L, "clock", state_of(s.groups, s.baseline, budyk::kGroupClock));
    set_number (L, "offset_ms",    s.clock.offset_ms);
    set_number (L, "max_error_ms", s.clock.max_error_ms);
    set_boolean(L, "synced",       s.clock.synced);
    set_boolean(L, "present",      s.clock.present);
    lua_pop(L, 1);

    lua_pushnumber(L, s.uptime_seconds);
    lua_setglobal(L, "uptime_seconds");
}

int budyk_lua_uncollected(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    int which = 0;
    for (const auto& k : { std::make_pair(kUncollectedKey, 1), std::make_pair(kBaselineKey, 2) }) {
        lua_getfield(L, LUA_REGISTRYINDEX, k.first);
        if (lua_type(L, -1) == LUA_TSTRING && lua_rawequal(L, -1, idx) != 0) which = k.second;
        lua_pop(L, 1);
    }
    return which;
}

namespace {

// collected(name): whether the sample being evaluated has group `name`
// ("disk", "self" or "self_", ...) or list `name` ("disk.devices", ...).
// An unknown name raises, so a typo does not read as "not collected".
int l_collected(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (std::strcmp(name, "self_") == 0) name = "self";
    const uint32_t group = budyk::metric_group_bit(name);
    const uint32_t list  = budyk::metric_detail_bit(name);
    if (group == 0 && list == 0) {
        return luaL_error(L, "collected: unknown metric '%s'", name);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, group != 0 ? "budyk.groups" : "budyk.lists");
    const lua_Integer have = lua_tointeger(L, -1);
    lua_pushboolean(L, (static_cast<uint64_t>(have) & (group != 0 ? group : list)) != 0);
    return 1;
}

} // namespace

void budyk_lua_register_collected(lua_State* L) {
    lua_register(L, "collected", l_collected);
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
