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

    // disk — aggregate throughput across whole block devices.
    lua_newtable(L);
    set_integer(L, "read_bytes_per_sec",  static_cast<lua_Integer>(s.disk.read_bytes_per_sec));
    set_integer(L, "write_bytes_per_sec", static_cast<lua_Integer>(s.disk.write_bytes_per_sec));
    set_integer(L, "device_count",        static_cast<lua_Integer>(s.disk.device_count));
    lua_setglobal(L, "disk");

    // net — aggregate throughput across non-loopback interfaces.
    lua_newtable(L);
    set_integer(L, "rx_bytes_per_sec",   static_cast<lua_Integer>(s.net.rx_bytes_per_sec));
    set_integer(L, "tx_bytes_per_sec",   static_cast<lua_Integer>(s.net.tx_bytes_per_sec));
    set_integer(L, "interface_count",    static_cast<lua_Integer>(s.net.interface_count));
    lua_setglobal(L, "net");

    // proc — running / total process counts.
    lua_newtable(L);
    set_integer(L, "total",   static_cast<lua_Integer>(s.proc.total));
    set_integer(L, "running", static_cast<lua_Integer>(s.proc.running));
    lua_setglobal(L, "proc");

    // entropy — kernel CSPRNG pool depth in bits (Linux only).
    lua_newtable(L);
    set_integer(L, "available_bits", static_cast<lua_Integer>(s.entropy.available_bits));
    lua_pushboolean(L, s.entropy.present);
    lua_setfield(L, -2, "present");
    lua_setglobal(L, "entropy");

    // self — daemon's own RSS / peak / CPU consumption.
    lua_newtable(L);
    set_integer(L, "rss_bytes",          static_cast<lua_Integer>(s.self_.rss_bytes));
    set_integer(L, "peak_rss_bytes",     static_cast<lua_Integer>(s.self_.peak_rss_bytes));
    set_number (L, "cpu_user_seconds",   s.self_.cpu_user_seconds);
    set_number (L, "cpu_system_seconds", s.self_.cpu_system_seconds);
    lua_setglobal(L, "self_");

    // thermal — hottest sensor reading across thermal_zone* / cpu.<N>.
    lua_newtable(L);
    set_number (L, "max_celsius",  s.thermal.max_celsius);
    set_integer(L, "sensor_count", static_cast<lua_Integer>(s.thermal.sensor_count));
    lua_pushboolean(L, s.thermal.present);
    lua_setfield(L, -2, "present");
    lua_setglobal(L, "thermal");

    lua_pushnumber(L, s.uptime_seconds);
    lua_setglobal(L, "uptime_seconds");
}
