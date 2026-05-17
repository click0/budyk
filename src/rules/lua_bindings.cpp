// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_bindings.h"

#include "core/sample.h"
#include "security/ssh_audit.h"

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

void budyk_lua_bind_ssh_audit(lua_State* L, const budyk::SshAuditStats& s) {
    // Expose cumulative counters + the single highest-hit IP/user so
    // rules can write `if ssh_audit.top_ip_hits > 20 then …`.
    // The full top_ips / top_users tables aren't projected — operators
    // who need richer queries can move to a follow-up that exposes
    // them as Lua sequences.
    lua_newtable(L);
    set_integer(L, "failed_password",
                static_cast<lua_Integer>(s.failed_password));
    set_integer(L, "invalid_user",
                static_cast<lua_Integer>(s.invalid_user));
    set_integer(L, "accepted",
                static_cast<lua_Integer>(s.accepted));

    if (!s.top_ips.empty()) {
        lua_pushstring(L, s.top_ips.front().first.c_str());
        lua_setfield(L, -2, "top_ip");
        set_integer(L, "top_ip_hits",
                    static_cast<lua_Integer>(s.top_ips.front().second));
    } else {
        lua_pushstring(L, "");
        lua_setfield(L, -2, "top_ip");
        set_integer(L, "top_ip_hits", 0);
    }
    if (!s.top_users.empty()) {
        lua_pushstring(L, s.top_users.front().first.c_str());
        lua_setfield(L, -2, "top_user");
        set_integer(L, "top_user_hits",
                    static_cast<lua_Integer>(s.top_users.front().second));
    } else {
        lua_pushstring(L, "");
        lua_setfield(L, -2, "top_user");
        set_integer(L, "top_user_hits", 0);
    }
    lua_setglobal(L, "ssh_audit");
}
