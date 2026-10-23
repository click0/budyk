// SPDX-License-Identifier: BSD-3-Clause
#include "core/sample.h"
#include "core/metric_set.h"
#include "rules/lua_engine.h"
#include "rules/alert.h"

#include <cassert>
#include <cerrno>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

using namespace budyk;

static Sample mk(double cpu_pct, double mem_avail_pct, double load1, double swap_used_pct) {
    Sample s{};
    s.groups                 = kGroupsAll;
    s.timestamp_nanos        = 1;
    s.level                  = Level::L3;
    s.cpu.total_percent      = cpu_pct;
    s.cpu.count              = 4;
    s.mem.available_percent  = mem_avail_pct;
    s.mem.total              = 16ULL << 30;
    s.mem.available          = 4ULL  << 30;
    s.swap.used_percent      = swap_used_pct;
    s.load.avg_1m            = load1;
    s.uptime_seconds         = 1234.5;
    return s;
}

static Sample mk_io(uint64_t disk_read, uint64_t disk_write,
                    uint64_t net_rx,    uint64_t net_tx) {
    Sample s                   = mk(0, 0, 0, 0);
    s.disk.read_bytes_per_sec  = disk_read;
    s.disk.write_bytes_per_sec = disk_write;
    s.disk.device_count        = 2;
    s.net.rx_bytes_per_sec     = net_rx;
    s.net.tx_bytes_per_sec     = net_tx;
    s.net.interface_count      = 3;
    return s;
}

int main() {
    // 1. Init / shutdown idempotent + rule_count starts at 0.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.rule_count() == 0);
        assert(e.last_fire_count() == 0);
        e.shutdown();

        // Re-init after shutdown works.
        assert(e.init(false) == 0);
        e.shutdown();
    }

    // 2. watch() registers a rule; eval_tick() returns fire count.
    {
        LuaEngine e;
        assert(e.init(false) == 0);

        const char* script =
            "watch('high_cpu', { when = function() return cpu.total_percent > 90 end })\n";
        assert(e.load_string(script) == 0);
        assert(e.rule_count() == 1);

        // Not firing.
        assert(e.eval_tick(mk(50.0, 80.0, 0.1, 0.0)) == 0);
        assert(e.last_fire_count() == 0);

        // Firing.
        assert(e.eval_tick(mk(95.0, 10.0, 2.5, 0.0)) == 1);
        assert(e.last_fire_count() == 1);
        assert(e.rules()[0].fire_count == 1);

        // Then a cooldown as long as for_ticks (spec §3.6.4): one tick.
        assert(e.rules()[0].cooldown_ticks == 1);
        assert(e.eval_tick(mk(95.0, 10.0, 2.5, 0.0)) == 0);

        // Firing again accumulates fire_count.
        assert(e.eval_tick(mk(95.0, 10.0, 2.5, 0.0)) == 1);
        assert(e.rules()[0].fire_count == 2);

        e.shutdown();
    }

    // 3. Multiple rules in one eval_tick, independent state.
    {
        LuaEngine e;
        assert(e.init(false) == 0);

        const char* script =
            "watch('cpu_hot',   { when = function() return cpu.total_percent > 80 end })\n"
            "watch('mem_low',   { when = function() return mem.available_percent < 5 end })\n"
            "watch('load_high', { when = function() return load.avg_1m > 4.0 end })\n";
        assert(e.load_string(script) == 0);
        assert(e.rule_count() == 3);

        // Only cpu_hot + load_high fire.
        assert(e.eval_tick(mk(85.0, 50.0, 5.0, 10.0)) == 2);

        // Only mem_low fires.
        assert(e.eval_tick(mk(10.0, 2.0, 0.1, 0.0)) == 1);

        e.shutdown();
    }

    // 4. Sandbox: io / os / require / loadfile / dofile / load are all nil.
    {
        LuaEngine e;
        assert(e.init(false) == 0);

        const char* script =
            "results = {}\n"
            "for _, name in ipairs({'io','os','require','loadfile','dofile','load'}) do\n"
            "  results[#results + 1] = tostring(_G[name] == nil)\n"
            "end\n"
            "all_nil = (table.concat(results, ',') == 'true,true,true,true,true,true')\n";
        assert(e.load_string(script) == 0);

        // Register a rule that exposes all_nil — simplest way to read back a boolean.
        assert(e.load_string(
            "watch('sandbox_check', { when = function() return all_nil == true end })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);

        e.shutdown();
    }

    // 5. Lua-function action fires side effects.
    {
        LuaEngine e;
        assert(e.init(false) == 0);

        const char* script =
            "counter = 0\n"
            "watch('bump', {\n"
            "  when   = function() return cpu.total_percent > 50 end,\n"
            "  action = function() counter = counter + 1 end,\n"
            "  cooldown = 0\n"
            "})\n"
            "watch('observer', { when = function() return counter >= 2 end })\n";
        assert(e.load_string(script) == 0);

        // First tick: bump fires, observer doesn't (counter=1).
        assert(e.eval_tick(mk(60, 0, 0, 0)) == 1);
        // Second tick: bump fires, observer fires (counter=2).
        assert(e.eval_tick(mk(60, 0, 0, 0)) == 2);

        e.shutdown();
    }

    // 6. exec() is rejected when not enabled.
    {
        LuaEngine e;
        assert(e.init(/*enable_exec*/ false) == 0);
        // A rule that calls exec() in its when() — pcall catches the error
        // and returns nil/false → rule doesn't fire, engine keeps running.
        assert(e.load_string(
            "watch('uses_exec', { when = function() exec('/bin/true'); return true end })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 0);

        e.shutdown();
    }

    // 7. Bad Lua rejected at load.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string("this is not valid lua !@#") != 0);
        assert(e.rule_count() == 0);
        e.shutdown();
    }

    // 8. for_ticks — rule requires N consecutive hits before it fires.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        const char* script =
            "watch('sustained', {\n"
            "  when      = function() return cpu.total_percent > 50 end,\n"
            "  for_ticks = 3,\n"
            "  cooldown  = 0,\n"
            "})\n";
        assert(e.load_string(script) == 0);

        // First two ticks do not fire — counter is still ramping.
        Sample hot = mk(60, 0, 0, 0);
        assert(e.eval_tick(hot) == 0);
        assert(e.rules()[0].consecutive_hits == 1);
        assert(e.eval_tick(hot) == 0);
        assert(e.rules()[0].consecutive_hits == 2);
        // Third consecutive true fires; counter resets.
        assert(e.eval_tick(hot) == 1);
        assert(e.rules()[0].consecutive_hits == 0);
        assert(e.rules()[0].fire_count       == 1);

        e.shutdown();
    }

    // 9. Breaking the streak resets consecutive_hits.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('streak', {\n"
            "  when = function() return cpu.total_percent > 50 end,\n"
            "  for_ticks = 3, cooldown = 0\n"
            "})\n") == 0);

        assert(e.eval_tick(mk(60, 0, 0, 0)) == 0);    // hit 1
        assert(e.eval_tick(mk(60, 0, 0, 0)) == 0);    // hit 2
        assert(e.eval_tick(mk(10, 0, 0, 0)) == 0);    // reset
        assert(e.rules()[0].consecutive_hits == 0);
        assert(e.eval_tick(mk(60, 0, 0, 0)) == 0);    // hit 1 again
        assert(e.rules()[0].fire_count       == 0);
        e.shutdown();
    }

    // 10. Cooldown: after fire, subsequent ticks are skipped for N ticks
    //     and the rule's action is not re-run.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "fires = 0\n"
            "watch('cd', {\n"
            "  when      = function() return cpu.total_percent > 50 end,\n"
            "  for_ticks = 1,\n"
            "  cooldown  = 3,\n"
            "  action    = function() fires = fires + 1 end,\n"
            "})\n") == 0);

        assert(e.eval_tick(mk(60, 0, 0, 0)) == 1);             // fire #1
        assert(e.rules()[0].cooldown_remaining == 3);
        // Next three ticks skipped — even though condition still true.
        assert(e.eval_tick(mk(60, 0, 0, 0)) == 0);
        assert(e.eval_tick(mk(60, 0, 0, 0)) == 0);
        assert(e.eval_tick(mk(60, 0, 0, 0)) == 0);
        assert(e.rules()[0].cooldown_remaining == 0);
        // Cooldown expired — next hit fires again.
        assert(e.eval_tick(mk(60, 0, 0, 0)) == 1);             // fire #2

        // The Lua-side action counter should be exactly fire_count.
        assert(e.load_string(
            "watch('observer', { when = function() return fires == 2 end })\n") == 0);
        assert(e.eval_tick(mk(10, 0, 0, 0)) == 1);
        e.shutdown();
    }

    // 11. for_ticks=0 is clamped to 1 (backwards-compatible with existing
    //     rules), and so is the cooldown it brings.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('default', { when = function() return true end, for_ticks = 0 })\n") == 0);
        assert(e.rules()[0].for_ticks == 1 && e.rules()[0].cooldown_ticks == 1);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        e.shutdown();
    }

    // 11b. exec(cmd, { wait = true }) in a rule — Lua gets a result table
    //      for /bin/true / /bin/false. Engine is initialised with
    //      enable_exec=true.
    {
        LuaEngine e;
        assert(e.init(/*enable_exec*/ true) == 0);
        const bool have_true  = access("/bin/true",  X_OK) == 0;
        const bool have_false = access("/bin/false", X_OK) == 0;

        if (have_true) {
            assert(e.load_string(
                "watch('true_ok', { when = function()\n"
                "  local r = exec('/bin/true', { timeout = 5, wait = true })\n"
                "  return r.ok == true and r.exit_status == 0 and r.timed_out == false\n"
                "end })\n") == 0);
            assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        }
        if (have_false) {
            assert(e.load_string(
                "watch('false_ok', { when = function()\n"
                "  local r = exec('/bin/false', { timeout = 5, wait = true })\n"
                "  return r.exit_status == 1 and r.ok == false\n"
                "end })\n") == 0);
            // 'true_ok' is in its cooldown: the new rule fires alone.
            assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        }
        e.shutdown();
    }

    // 11c. exec() with an argv table — exec({'/bin/sh', '-c', 'exit 7'}, 5).
    {
        if (access("/bin/sh", X_OK) == 0) {
            LuaEngine e;
            assert(e.init(/*enable_exec*/ true) == 0);
            assert(e.load_string(
                "watch('sh7', { when = function()\n"
                "  local r = exec({'/bin/sh', '-c', 'exit 7'}, { timeout = 5, wait = true })\n"
                "  return r.exit_status == 7 and r.ok == false\n"
                "end })\n") == 0);
            assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
            e.shutdown();
        }
    }

    // 11d. Hardening — exec('true') without absolute path is rejected.
    {
        LuaEngine e;
        assert(e.init(/*enable_exec*/ true) == 0);
        assert(e.load_string(
            "ok, err = pcall(exec, 'true', 5)\n"
            "blocked = (ok == false and tostring(err):find('absolute') ~= nil)\n"
            "watch('no_relative', { when = function() return blocked end })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        e.shutdown();
    }

    // 11e. Hardening — path traversal rejected ("/bin/../bin/true").
    {
        LuaEngine e;
        assert(e.init(/*enable_exec*/ true) == 0);
        assert(e.load_string(
            "ok, err = pcall(exec, '/bin/../bin/true', 5)\n"
            "blocked = (ok == false and tostring(err):find('traversal') ~= nil)\n"
            "watch('no_dotdot', { when = function() return blocked end })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        e.shutdown();
    }

    // 11f. Hardening — allowlist denies /bin/true when only /bin/echo listed.
    {
        LuaEngine e;
        assert(e.init(/*enable_exec*/ true) == 0);
        e.set_exec_allowlist({"/bin/echo"});
        assert(e.load_string(
            "ok, err = pcall(exec, '/bin/true', 5)\n"
            "blocked = (ok == false and tostring(err):find('allowlist') ~= nil)\n"
            "watch('denied', { when = function() return blocked end })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        e.shutdown();
    }

    // 11g. Hardening — allowlist permits exactly-matching path.
    if (access("/bin/true", X_OK) == 0) {
        LuaEngine e;
        assert(e.init(/*enable_exec*/ true) == 0);
        e.set_exec_allowlist({"/bin/true", "/bin/echo"});
        assert(e.load_string(
            "watch('allowed', { when = function()\n"
            "  local r = exec('/bin/true', { timeout = 5, wait = true })\n"
            "  return r.ok == true and r.exit_status == 0\n"
            "end })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        e.shutdown();
    }

    // 12. Disk + net bindings — rules reference `disk.*` and `net.*` tables.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        const char* script =
            "watch('disk_hot', { when = function() return disk.read_bytes_per_sec > 10000000 end, cooldown = 0 })\n"
            "watch('net_hot',  { when = function() return net.tx_bytes_per_sec  > 1000000  end, cooldown = 0 })\n"
            "watch('iface_ok', { when = function() return net.interface_count   >= 2       end, cooldown = 0 })\n"
            "watch('devs_ok',  { when = function() return disk.device_count     >= 2       end, cooldown = 0 })\n";
        assert(e.load_string(script) == 0);
        assert(e.rule_count() == 4);

        // Idle — only iface_ok + devs_ok fire (2 IFs ≥ 2, 2 devs ≥ 2).
        assert(e.eval_tick(mk_io(0, 0, 0, 0)) == 2);

        // Disk hot + net hot also fire.
        assert(e.eval_tick(mk_io(20ULL*1024*1024, 0, 0, 5ULL*1024*1024)) == 4);

        // Only net_hot — disk quiet.
        assert(e.eval_tick(mk_io(0, 0, 0, 5ULL*1024*1024)) == 3);

        e.shutdown();
    }

    // 16. freeze() is disabled by default — calling it from a rule
    //     should raise an error and load_string therefore fails.
    {
        budyk::LuaEngine e;
        assert(e.init(/*enable_exec=*/false) == 0);
        // Run freeze() at file scope so the error surfaces from
        // load_string. We use pid 1 — gate runs before the actual kill.
        const int rc = e.load_string("freeze(1)\n");
        assert(rc != 0);   // luaL_error → load_string returns -2
        e.shutdown();
    }

    // 17. freeze() with engine.set_freeze_enabled(true) but the target's
    //     comm not in the allowlist → also rejected at gate time, never
    //     touches kill(). We point freeze() at our own pid (we know the
    //     name resolves) but list a name that doesn't match.
    {
        budyk::LuaEngine e;
        assert(e.init(false) == 0);
        e.set_freeze_enabled(true);
        e.set_freeze_allowlist({"this-name-definitely-does-not-match"});
        char buf[128];
        std::snprintf(buf, sizeof(buf), "freeze(%d)\n",
                      static_cast<int>(::getpid()));
        // load_string runs at the file scope; the gate error surfaces
        // as a luaL_error, which dostring reports as a non-zero rc.
        const int rc = e.load_string(buf);
        assert(rc != 0);
        e.shutdown();
    }

    // 18. `files` global is bound when set_file_state() has been
    //     called; rules can read .modifies / .tampered.
    {
        budyk::LuaEngine e;
        assert(e.init(false) == 0);

        budyk::FileWatchState st;
        st.modifies["/etc/sudoers"]            = 3;
        st.deletes ["/var/log/audit"]          = 1;
        st.tampered_this_tick.insert("/etc/sudoers");
        e.set_file_state(st);

        assert(e.load_string(R"(
            watch("sudoers_tamper", {
                when = function()
                    local f = files["/etc/sudoers"]
                    return f
                       and f.modifies == 3
                       and f.tampered == true
                end,
            })
        )") == 0);

        budyk::Sample s{};
        assert(e.eval_tick(s) == 1);
        e.shutdown();
    }

    // 21. tampered_this_tick is cleared between ticks — a rule that
    //     fires on .tampered won't keep firing forever once the event
    //     drains. We seed a tampered path, eval once (fires), then
    //     hand in an "empty events" apply() and eval again (no fire).
    {
        budyk::LuaEngine e;
        assert(e.init(false) == 0);

        budyk::FileWatchState st;
        st.modifies["/etc/sudoers"] = 1;
        st.tampered_this_tick.insert("/etc/sudoers");
        e.set_file_state(st);

        assert(e.load_string(R"(
            watch("once", {
                when = function()
                    local f = files["/etc/sudoers"]
                    return f and f.tampered == true
                end,
            })
        )") == 0);

        budyk::Sample s{};
        assert(e.eval_tick(s) == 1);

        // Simulate a tick with no events — daemon would call
        // st.apply({}) which clears tampered_this_tick.
        st.apply({});
        e.set_file_state(st);
        assert(e.eval_tick(s) == 0);

        e.shutdown();
    }

    // 22. save_state / load_state — a rule mid-cooldown survives a
    //     simulated restart, so it does NOT re-fire immediately.
    {
        char tmpl[] = "/tmp/budyk_state_XXXXXX";
        int  fd     = ::mkstemp(tmpl);
        assert(fd >= 0);
        ::close(fd);

        const char* rule =
            "watch('hot', {\n"
            "  when = function() return cpu.total_percent > 90 end,\n"
            "  for_ticks = 1,\n"
            "  cooldown  = 5,\n"
            "})\n";

        // First engine: fire the rule once → cooldown_remaining = 5.
        {
            LuaEngine e;
            assert(e.init(false) == 0);
            assert(e.load_string(rule) == 0);
            assert(e.eval_tick(mk(95, 0, 0, 0)) == 1);   // fires
            assert(e.eval_tick(mk(95, 0, 0, 0)) == 0);   // in cooldown
            assert(e.rules()[0].cooldown_remaining > 0);
            assert(e.rules()[0].fire_count == 1);
            assert(e.save_state(tmpl) == 0);
            e.shutdown();
        }

        // Second engine ("after restart"): same rule, restore state.
        // The very next tick must NOT fire because cooldown persisted.
        {
            LuaEngine e;
            assert(e.init(false) == 0);
            assert(e.load_string(rule) == 0);
            assert(e.load_state(tmpl) == 0);
            assert(e.rules()[0].cooldown_remaining > 0);
            assert(e.rules()[0].fire_count == 1);        // counter restored
            // Condition is still hot, but cooldown blocks the fire.
            assert(e.eval_tick(mk(95, 0, 0, 0)) == 0);
            e.shutdown();
        }

        ::unlink(tmpl);
    }

    // 23. load_state on a missing file is a no-op success (fresh box).
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_state("/tmp/budyk_state_does_not_exist_zz") == 0);
        e.shutdown();
    }

    // 24. Alert channels survive shutdown()/init() — the dispatcher is a
    //     LuaEngine member, not tied to the Lua state. This is the exact
    //     invariant the SIGHUP reload path relies on: it must NOT
    //     re-register channels after the swap, or they duplicate and each
    //     alert fires N+1 times after N reloads. Regression guard for the
    //     channel-dup bug shipped in the initial SIGHUP PR.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        AlertChannel ch;
        ch.name = "test";
        ch.type = "ntfy";
        ch.url  = "http://127.0.0.1:0";
        ch.topic = "t";
        e.alerts().add_channel(ch);
        assert(e.alerts().channel_count() == 1);

        // Simulate a SIGHUP reload: save state, tear down the Lua VM,
        // re-init. The dispatcher (and its channels) must be untouched.
        e.shutdown();
        assert(e.alerts().channel_count() == 1);   // survives shutdown
        assert(e.init(false) == 0);
        assert(e.alerts().channel_count() == 1);   // survives re-init
        e.shutdown();
    }

    // 25. Default action (no `action`) sends alert(name, severity, message)
    //     using the rule's severity / message. Before this was fixed, the
    //     "alert" tag was stored but never acted on: rules fired, nothing
    //     was sent.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('hot', {\n"
            "  when     = function() return cpu.total_percent > 90 end,\n"
            "  severity = 'critical',\n"
            "  message  = 'CPU pegged',\n"
            "})\n") == 0);
        assert(e.eval_tick(mk(95, 50, 1, 0)) == 1);
        assert(e.alerts().dispatch_calls() == 1);
        assert(e.alerts().last_event().severity == AlertSeverity::Critical);
        assert(e.alerts().last_event().rule     == "hot");
        assert(e.alerts().last_event().message  == "CPU pegged");
        // Not firing → nothing sent.
        assert(e.eval_tick(mk(10, 50, 1, 0)) == 0);
        assert(e.alerts().dispatch_calls() == 1);
        e.shutdown();
    }

    // 26. `action = alert` (the builtin itself, the form the shipped
    //     examples and the old README used) means the "alert" action.
    //     Previously alert() was called with no arguments, raised inside
    //     pcall, and nothing was sent. Message defaults to the rule name;
    //     severity defaults to warning.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('bare', { when = function() return true end,"
            "                action = alert })\n") == 0);
        assert(e.eval_tick(mk(0, 50, 0, 0)) == 1);
        assert(e.alerts().dispatch_calls() == 1);
        assert(e.alerts().last_event().severity == AlertSeverity::Warning);
        assert(e.alerts().last_event().message  == "bare");
        e.shutdown();
    }

    // 27. action = "log" fires without dispatching an alert; a function
    //     action is still called and does its own thing.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "hits = 0\n"
            "watch('l', { when = function() return true end, action = 'log' })\n"
            "watch('f', { when = function() return true end,\n"
            "             action = function() hits = hits + 1 end })\n") == 0);
        assert(e.eval_tick(mk(0, 50, 0, 0)) == 2);
        assert(e.alerts().dispatch_calls() == 0);
        assert(e.load_string("assert(hits == 1)") == 0);
        e.shutdown();
    }

    // 28. Invalid rules fail at load time instead of silently doing
    //     nothing: an unknown action string, alone or in a list, an empty
    //     list, an unknown severity, a non-string message, a for_ticks or
    //     cooldown that is not a whole number in range. None of them
    //     registers a rule.
    {
        const char* bad[] = {
            "watch('t', { when = function() return true end, action = 'page' })",
            "watch('t', { when = function() return true end, action = { alert, 'page' } })",
            "watch('t', { when = function() return true end, action = { alert, 42 } })",
            "watch('t', { when = function() return true end, action = {} })",
            "watch('t', { when = function() return true end, action = 42 })",
            "watch('t', { when = function() return true end, severity = 'high' })",
            "watch('t', { when = function() return true end, message = 42 })",
            "watch('t', { when = function() return true end, for_ticks = 2.5 })",
            "watch('t', { when = function() return true end, for_ticks = -1 })",
            "watch('t', { when = function() return true end, for_ticks = 'x' })",
            "watch('t', { when = function() return true end, for_ticks = 1000001 })",
            "watch('t', { when = function() return true end, cooldown = -1 })",
            "watch('t', { when = function() return true end, cooldown = 0.5 })",
            "watch('t', { when = function() return true end, cooldown = {} })",
        };
        for (const char* code : bad) {
            LuaEngine e;
            assert(e.init(false) == 0);
            assert(e.load_string(code) != 0);
            assert(e.rule_count() == 0);
            // The error text is kept so the daemon can log it.
            assert(std::strstr(e.last_error().c_str(), "watch(t):") != nullptr);
            e.shutdown();
        }
    }

    // 29. severity is case-insensitive. cooldown defaults to for_ticks
    //     (spec §3.6.4): a rule that keeps holding fires once per
    //     2 * for_ticks ticks; cooldown = 0 fires on every hot tick.
    //     A whole number given as a string or a float counts.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('i', { when = function() return true end,"
            "             severity = 'INFO' })\n"
            "watch('three', { when = function() return true end, for_ticks = 3, action = 'log' })\n"
            "watch('every', { when = function() return true end, cooldown = 0, action = 'log' })\n"
            "watch('str', { when = function() return false end, for_ticks = '4', cooldown = 2.0 })\n") == 0);
        assert(e.rules()[0].cooldown_ticks == 1);
        assert(e.rules()[1].for_ticks == 3 && e.rules()[1].cooldown_ticks == 3);
        assert(e.rules()[2].cooldown_ticks == 0);
        assert(e.rules()[3].for_ticks == 4 && e.rules()[3].cooldown_ticks == 2);
        int fires_i = 0, fires_three = 0, fires_every = 0;
        for (int t = 0; t < 12; ++t) {
            e.eval_tick(mk(0, 50, 0, 0));
            fires_i     = static_cast<int>(e.rules()[0].fire_count);
            fires_three = static_cast<int>(e.rules()[1].fire_count);
            fires_every = static_cast<int>(e.rules()[2].fire_count);
        }
        assert(fires_i == 6 && fires_three == 2 && fires_every == 12);
        assert(e.alerts().dispatch_calls() == 6);
        assert(e.alerts().last_event().severity == AlertSeverity::Info);
        e.shutdown();
    }

    // 30. Custom-level conditions: compiled as `return (<expr>)` against
    //     the sample; true ones are reported by id, a compile error is
    //     reported, a runtime error counts as false.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.add_level_condition(4, "cpu.total_percent > 90") == 0);
        assert(e.add_level_condition(5, "load.avg_1m >= 0") == 0);
        assert(e.add_level_condition(6, "nosuch.field > 1") == 0);   // raises at runtime
        assert(e.add_level_condition(7, "cpu.total_percent >") == -2);
        assert(!e.last_error().empty());
        std::vector<uint8_t> active;
        e.eval_level_conditions(mk(95, 50, 1, 0), &active);
        assert(active.size() == 2 && active[0] == 4 && active[1] == 5);
        e.eval_level_conditions(mk(10, 50, 1, 0), &active);
        assert(active.size() == 1 && active[0] == 5);
        e.shutdown();
    }

    // 31. escalate(level, seconds): known names (case-insensitive) queue a
    //     request that take_escalations() hands over once; an unknown
    //     level or a bad duration raises, so the action fails and nothing
    //     is queued.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        e.set_level_names({"L1", "L2", "L3", "burst"});
        assert(e.load_string(
            "watch('a', { when = function() return true end,"
            "             action = function() escalate('BURST', 5) end, cooldown = 0 })\n"
            "watch('b', { when = function() return true end,"
            "             action = function() escalate('L3') end, cooldown = 0 })\n"
            "watch('c', { when = function() return true end,"
            "             action = function() escalate('nope', 5) end, cooldown = 0 })\n"
            "watch('d', { when = function() return true end,"
            "             action = function() escalate('burst', 0) end, cooldown = 0 })\n") == 0);
        assert(e.eval_tick(mk(0, 50, 0, 0)) == 4);
        auto esc = e.take_escalations();
        assert(esc.size() == 2);
        assert(esc[0].level == "BURST" && esc[0].seconds == 5);
        assert(esc[1].level == "L3"    && esc[1].seconds == 60);   // default
        assert(e.take_escalations().empty());
        e.shutdown();
    }

    // 32. Instruction limit: a when() that never returns is cut off with
    //     an error on that rule only; the other rules still evaluate,
    //     and the next tick runs normally. load_string gets the same
    //     treatment for a loop at file level.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.instruction_limit() == LuaEngine::kDefaultInstructionLimit);
        e.set_limits(200000, LuaEngine::kDefaultMemoryLimit);
        assert(e.instruction_limit() == 200000);

        assert(e.load_string(
            "watch('spin', { when = function() while true do end end, cooldown = 0 })\n"
            "watch('ok',   { when = function() return cpu.total_percent > 50 end, cooldown = 0 })\n") == 0);
        assert(e.rule_count() == 2);

        for (int tick = 0; tick < 3; ++tick) {
            assert(e.eval_tick(mk(90.0, 50.0, 0.1, 0.0)) == 1);   // only 'ok'
            assert(e.rules()[0].last_error.find("instruction limit") != std::string::npos);
            assert(e.rules()[0].fire_count == 0);
            assert(e.rules()[1].fire_count == static_cast<uint64_t>(tick + 1));
        }

        // A runaway action is contained the same way: the rule still
        // counts as fired, the error is recorded, the tick completes.
        assert(e.load_string(
            "watch('spin_action', { when = function() return true end,"
            " action = function() local n = 0 while true do n = n + 1 end end, cooldown = 0 })\n") == 0);
        assert(e.eval_tick(mk(90.0, 50.0, 0.1, 0.0)) == 2);
        assert(e.rules()[2].fire_count == 1);
        assert(e.rules()[2].last_error.find("instruction limit") != std::string::npos);

        // Top-level loop in the rules file.
        assert(e.load_string("local n = 0 while true do n = n + 1 end\n") == -2);
        assert(e.last_error().find("instruction limit") != std::string::npos);
        assert(e.rule_count() == 3);                 // engine still usable
        assert(e.eval_tick(mk(10.0, 50.0, 0.1, 0.0)) == 1);   // spin_action only

        e.shutdown();
    }

    // 33. Memory limit: a rule that allocates without bound gets "not
    //     enough memory" instead of taking the daemon down; what it
    //     allocated is released, and the engine keeps working.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.memory_limit() == LuaEngine::kDefaultMemoryLimit);
        const size_t base = e.memory_used();
        assert(base > 0 && base < (1u << 20));      // libs + stdlib, well under 1 MiB

        e.set_limits(LuaEngine::kDefaultInstructionLimit, 2u << 20);   // 2 MiB
        assert(e.load_string(
            "watch('hog', { when = function()\n"
            "  local t = {}\n"
            "  for i = 1, 1000000 do t[i] = ('x'):rep(1024) end\n"
            "  return true\n"
            "end, cooldown = 0 })\n"
            "watch('ok', { when = function() return true end, cooldown = 0 })\n") == 0);

        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);   // 'ok' fires, 'hog' errors
        assert(e.rules()[0].last_error.find("not enough memory") != std::string::npos);
        assert(e.memory_used() <= (2u << 20));

        // One huge string from a single C call: no VM instructions to
        // count, so this is the memory limit's job alone.
        assert(e.load_string(
            "watch('big', { when = function() local s = ('x'):rep(64 * 1024 * 1024) return true end, cooldown = 0 })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        assert(e.rules()[2].last_error.find("not enough memory") != std::string::npos);

        // After a full collection the state is back near its baseline:
        // nothing leaked past the refused allocations.
        assert(e.load_string("collectgarbage('collect')") == 0);
        assert(e.memory_used() < base + (256u << 10));

        e.shutdown();
        assert(e.memory_used() == 0);
    }

    // 34. Level conditions are under the same instruction limit: a
    //     looping condition counts as false and is logged, the others
    //     still evaluate.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        e.set_limits(100000, LuaEngine::kDefaultMemoryLimit);
        assert(e.add_level_condition(4, "(function() while true do end end)()") == 0);
        assert(e.add_level_condition(5, "cpu.total_percent > 50") == 0);
        std::vector<uint8_t> active;
        e.eval_level_conditions(mk(90.0, 50.0, 0.1, 0.0), &active);
        assert(active.size() == 1 && active[0] == 5);
        e.shutdown();
    }

    // 35. exec() without wait runs the command on the engine's worker:
    //     the call (and the tick) returns at once with { queued = true },
    //     the command still runs to completion, and a reload
    //     (shutdown + init) neither waits for it nor loses it.
    if (access("/bin/sh", X_OK) == 0) {
        char tmpl[] = "/tmp/budyk_exec_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        assert(dir != nullptr);
        const std::string marker = std::string(dir) + "/done";

        LuaEngine e;
        assert(e.init(/*enable_exec*/ true) == 0);
        const std::string bg =
            "watch('bg', { when = function()\n"
            "  r = exec({'/bin/sh', '-c', 'sleep 0.3; touch " + marker + "'})\n"
            "  return r.queued == true and r.ok == true\n"
            "end, cooldown = 0 })\n";
        assert(e.load_string(bg.c_str()) == 0);
        const auto t0 = std::chrono::steady_clock::now();
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        const double secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        assert(secs < 0.25);                          // did not wait 0.3 s
        assert(access(marker.c_str(), F_OK) != 0);    // not done yet

        e.shutdown();                                 // as a SIGHUP reload does
        assert(e.init(/*enable_exec*/ true) == 0);
        assert(e.exec_worker().wait_idle(5000));
        assert(e.exec_worker().completed() == 1);
        assert(access(marker.c_str(), F_OK) == 0);    // it ran

        // wait = true caps the timeout at a minute and the result shows
        // a kill; a plain number as the second argument means a timeout.
        assert(e.load_string(
            "watch('bg2', { when = function()\n"
            "  local r2 = exec({'/bin/sh', '-c', 'sleep 5'}, { timeout = 1, wait = true })\n"
            "  local r3 = exec('/bin/true', 5)\n"
            "  return r2.timed_out == true and r2.ok == false and r3.queued == true\n"
            "end, cooldown = 0 })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) >= 1);
        assert(e.exec_worker().wait_idle(5000));
        assert(e.exec_worker().completed() == 2);
        e.shutdown();
        ::unlink(marker.c_str());
        ::rmdir(dir);
    }

    // 36. The exec() allowlist is compared by realpath, resolved when the
    //     list is set: a symlink to an allowed binary and a path with
    //     "//" are allowed; an allowed path that is later re-pointed at
    //     another binary is not.
    if (access("/bin/true", X_OK) == 0 && access("/bin/false", X_OK) == 0) {
        char tmpl[] = "/tmp/budyk_allow_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        assert(dir != nullptr);
        const std::string link = std::string(dir) + "/cmd";
        assert(::symlink("/bin/true", link.c_str()) == 0);

        auto run = [](LuaEngine& e, const std::string& cmd) {
            // One rule whose when() runs the command inline; a rejected
            // exec() raises, which eval_tick logs and counts as no hit.
            const std::string src =
                "watch('x', { when = function() local r = exec('" + cmd +
                "', { timeout = 5, wait = true }) return r.ok == true end, cooldown = 0 })\n";
            assert(e.load_string(src.c_str()) == 0);
            const int fired = e.eval_tick(mk(0, 0, 0, 0));
            const std::string err = e.rules().back().last_error;
            e.shutdown();
            return std::make_pair(fired, err);
        };

        // Allowed by resolved identity, not by spelling.
        {
            LuaEngine e;
            assert(e.init(true) == 0);
            e.set_exec_allowlist({"/bin/true"});
            auto [fired, err] = run(e, link);                 // symlink -> /bin/true
            assert(fired == 1 && err.empty());
        }
        {
            LuaEngine e;
            assert(e.init(true) == 0);
            e.set_exec_allowlist({"/bin/true"});
            auto [fired, err] = run(e, "/bin//true");
            assert(fired == 1 && err.empty());
        }
        // The allowed entry is the symlink; it resolved to /bin/true when
        // set. Re-point it at /bin/false: the command's realpath no
        // longer matches, so it is refused even though the spelling is
        // exactly the allowed one.
        {
            LuaEngine e;
            assert(e.init(true) == 0);
            e.set_exec_allowlist({link});
            assert(::unlink(link.c_str()) == 0);
            assert(::symlink("/bin/false", link.c_str()) == 0);
            auto [fired, err] = run(e, link);
            assert(fired == 0);
            assert(err.find("not in allowlist") != std::string::npos);
        }
        ::unlink(link.c_str());
        ::rmdir(dir);
    }

    // 37. Sample tables are read-only proxies, created once and updated in
    //     place (spec §3.6, review M6):
    //     - assigning a field raises, names the field, and the next rule
    //       still sees the real value;
    //     - the global is the same table from tick to tick, with new values;
    //     - pairs() iterates the real fields; getmetatable() says
    //       "read-only" and setmetatable() cannot replace it;
    //     - a rule that replaces the global, or rawset()s onto the proxy,
    //       affects that tick at most.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "tick = 0\n"
            "watch('writer', { when = function() cpu.total_percent = 0 return true end, cooldown = 0 })\n"
            "watch('reader', { when = function() return cpu.total_percent > 50 end, cooldown = 0 })\n"
            "watch('same', { when = function()\n"
            "  tick = tick + 1\n"
            "  if tick == 1 then first = cpu return false end\n"
            "  return rawequal(cpu, first) and cpu.total_percent == 20\n"
            "end, cooldown = 0 })\n"
            "watch('pairs', { when = function()\n"
            "  local n = 0\n"
            "  for k, v in pairs(cpu) do n = n + 1 end\n"
            "  return n == 9\n"           // 8 numbers and cpu.cores
            "end, cooldown = 0 })\n"
            "watch('meta', { when = function()\n"
            "  local ok = pcall(setmetatable, cpu, nil)\n"
            "  return getmetatable(cpu) == 'read-only' and not ok\n"
            "end, cooldown = 0 })\n") == 0);

        // Tick 1: cpu 90. 'writer' errors; 'reader' still sees 90.
        assert(e.eval_tick(mk(90.0, 50.0, 0.1, 0.0)) == 3);   // reader, pairs, meta
        assert(e.rules()[0].fire_count == 0);
        assert(e.rules()[0].last_error.find("cpu.total_percent is read-only") != std::string::npos);
        // ...prefixed with the rule's chunk and line, like any Lua error.
        assert(e.rules()[0].last_error.find("]:2: cpu.total_percent") != std::string::npos);
        assert(e.rules()[1].fire_count == 1);
        assert(e.rules()[2].fire_count == 0);                  // stashed the table
        // Tick 2: cpu 20. The same table, the new value.
        assert(e.eval_tick(mk(20.0, 50.0, 0.1, 0.0)) == 3);   // same, pairs, meta
        assert(e.rules()[1].fire_count == 1);                  // 20 is not > 50
        assert(e.rules()[2].fire_count == 1);
        assert(e.rules()[3].fire_count == 2 && e.rules()[4].fire_count == 2);
        e.shutdown();
    }
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "t = 0\n"
            "watch('clobber', { when = function()\n"
            "  t = t + 1\n"
            "  if t == 1 then rawset(cpu, 'total_percent', -1) mem = nil end\n"
            "  return false\n"
            "end, cooldown = 0 })\n"
            "watch('see', { when = function()\n"
            "  seen_cpu = cpu.total_percent\n"
            "  seen_mem = mem and mem.available_percent\n"
            "  return true\n"
            "end, cooldown = 0 })\n") == 0);
        // Tick 1: the bypasses work within the tick...
        assert(e.load_string("seen_cpu = nil seen_mem = nil") == 0);
        assert(e.eval_tick(mk(90.0, 25.0, 0.1, 0.0)) == 1);
        assert(e.load_string("assert(seen_cpu == -1 and seen_mem == nil)") == 0);
        // ...and are gone on the next one.
        assert(e.eval_tick(mk(70.0, 30.0, 0.1, 0.0)) == 1);
        assert(e.load_string("assert(seen_cpu == 70 and seen_mem == 30)") == 0);
        e.shutdown();
    }

    // N. The v8 metrics reach rules: the CPU parts, process states, tcp,
    //    udp, fs and clock, read-only like every sample group.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('iowait', { when = function() return cpu.iowait_percent > 20 end, cooldown = 0 })\n"
            "watch('zombies', { when = function() return proc.zombie > 3 and proc.threads == 900 end, cooldown = 0 })\n"
            "watch('retrans', { when = function() return tcp.present and tcp.retransmits_per_sec > 10 end, cooldown = 0 })\n"
            "watch('udp', { when = function() return udp.no_port_per_sec > 1 end, cooldown = 0 })\n"
            "watch('disk_full', { when = function() return fs.max_used_percent >= 95 end, cooldown = 0 })\n"
            "watch('clock', { when = function() return clock.present and not clock.synced end, cooldown = 0 })\n"
            "watch('ro', { when = function() fs.max_used_percent = 0 return true end, cooldown = 0 })\n") == 0);
        Sample x = mk(10.0, 50.0, 0.1, 0.0);
        x.cpu.iowait_percent       = 25.0;
        x.proc.zombie              = 4;
        x.proc.threads             = 900;
        x.tcp.present              = true;
        x.tcp.retransmits_per_sec  = 12.5;
        x.udp.present              = true;
        x.udp.no_port_per_sec      = 3.0;
        x.fs.present               = true;
        x.fs.max_used_percent      = 96.0;
        x.clock.present            = true;
        x.clock.synced             = false;
        assert(e.eval_tick(x) == 6);
        assert(e.rules()[6].last_error.find("fs.max_used_percent is read-only") != std::string::npos);
        x.fs.max_used_percent = 50.0;
        x.clock.synced        = true;
        assert(e.eval_tick(x) == 4);
        e.shutdown();
    }

    // N+1. The per-entity detail reaches rules as read-only collections:
    //      cpu.cores (an array: # and ipairs work), disk.devices,
    //      net.interfaces and fs.mounts (by name). A device that is gone
    //      is gone from its table; without detail none is collected. Item
    //      tables are reused, and dropped with the item, so names that
    //      come and go (veths) do not grow the state.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('cores', { when = function()\n"
            "  local n = 0\n"
            "  for i, c in ipairs(cpu.cores) do n = n + 1 end\n"
            "  return #cpu.cores == 2 and n == 2 and cpu.cores[2].iowait_percent == 30\n"
            "end, cooldown = 0 })\n"
            "watch('disk', { when = function()\n"
            "  return disk.devices.sda ~= nil and disk.devices.sda.busy_percent > 50\n"
            "end, cooldown = 0 })\n"
            "watch('nic', { when = function()\n"
            "  return net.interfaces.eth0 ~= nil and net.interfaces.eth0.up\n"
            "end, cooldown = 0 })\n"
            "watch('full', { when = function()\n"
            "  for mnt, m in pairs(fs.mounts) do\n"
            "    if m.used_percent > 90 and m.type == 'ext4' then return true end\n"
            "  end\n"
            "  return false\n"
            "end, cooldown = 0 })\n"
            "watch('ro_item', { when = function() cpu.cores[1].user_percent = 0 return true end, cooldown = 0 })\n"
            "watch('ro_coll', { when = function() fs.mounts['/x'] = 1 return true end, cooldown = 0 })\n") == 0);

        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        d->collected = kDetailAll;
        d->ncores = 2;
        d->cores[1].iowait_percent = 30.0;
        d->ndisks = 1;
        std::snprintf(d->disks[0].name, sizeof(d->disks[0].name), "sda");
        d->disks[0].busy_percent = 75.0;
        d->nifaces = 1;
        std::snprintf(d->ifaces[0].name, sizeof(d->ifaces[0].name), "eth0");
        d->ifaces[0].up = 1;
        d->nmounts = 2;
        std::snprintf(d->mounts[0].mount, sizeof(d->mounts[0].mount), "/");
        std::snprintf(d->mounts[0].type,  sizeof(d->mounts[0].type),  "ext4");
        d->mounts[0].used_percent = 95.0;
        std::snprintf(d->mounts[1].mount, sizeof(d->mounts[1].mount), "/var");
        std::snprintf(d->mounts[1].type,  sizeof(d->mounts[1].type),  "xfs");
        const Sample x = mk(10.0, 50.0, 0.1, 0.0);
        assert(e.eval_tick(x, d.get()) == 4);
        assert(e.rules()[4].last_error.find("cpu.cores[1].user_percent is read-only") != std::string::npos);
        assert(e.rules()[5].last_error.find("fs.mounts./x is read-only") != std::string::npos);

        // sda went away, sdb came; the mount filled less.
        std::snprintf(d->disks[0].name, sizeof(d->disks[0].name), "sdb");
        d->mounts[0].used_percent = 50.0;
        assert(e.eval_tick(x, d.get()) == 2);   // cores, nic
        assert(e.eval_tick(x, nullptr) == 0);   // no detail: no list collected, all skip

        // Interface names that change every tick: the state does not grow.
        for (int i = 0; i < 200; ++i) {
            std::snprintf(d->ifaces[0].name, sizeof(d->ifaces[0].name), "veth%d", i);
            e.eval_tick(x, d.get());
        }
        const size_t after_200 = e.memory_used();
        for (int i = 200; i < 2200; ++i) {
            std::snprintf(d->ifaces[0].name, sizeof(d->ifaces[0].name), "veth%d", i);
            e.eval_tick(x, d.get());
        }
        assert(e.memory_used() < after_200 + 64 * 1024);
        e.shutdown();
    }

    // N+2. A group or list the sample's level did not collect (spec
    //      §3.6.5): reading it, its length or pairs() over it raises "<name>
    //      is not collected at this level"; a when() that does is skipped,
    //      its sustain count kept, the skip logged once (last_skip), no
    //      error. collected(name) tells. An action that reads it fails
    //      like any other. A rule may still pcall() around it. A level
    //      condition that reads it is false. Once collected, it reads again.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('io', { for_ticks = 3, when = function()\n"
            "  return disk.read_bytes_per_sec > 100\n"
            "end, cooldown = 0 })\n"
            "watch('len', { when = function() return #cpu.cores >= 0 end, cooldown = 0 })\n"
            "watch('iter', { when = function() for k in pairs(tcp) do end return true end, cooldown = 0 })\n"
            "watch('guard', { when = function()\n"
            "  return collected('disk') and collected('disk.devices') and collected('self_')\n"
            "end, cooldown = 0 })\n"
            "watch('typo', { when = function() return collected('dsik') end, cooldown = 0 })\n"
            "watch('act', { when = function() return true end,\n"
            "               action = function() local x = net.rx_bytes_per_sec end, cooldown = 0 })\n"
            "watch('caught', { when = function()\n"
            "  local ok, err = pcall(function() return disk.read_bytes_per_sec end)\n"
            "  return not ok and err:find('disk is not collected at this level') ~= nil\n"
            "end, cooldown = 0 })\n") == 0);
        assert(e.add_level_condition(kFirstCustomLevel, "disk.read_bytes_per_sec > 100") == 0);
        assert(e.add_level_condition(kFirstCustomLevel + 1, "cpu.total_percent >= 0") == 0);

        Sample full = mk_io(500, 0, 0, 0);
        Sample bare = full;
        clear_groups(&bare, kGroupsAll & ~kGroupsMinimal);   // an L1 with metrics: minimal
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        d->collected = kDetailAll;
        std::unique_ptr<budyk_detail_c> none(new budyk_detail_c());

        // full, bare, full, full: 'io' fires on the 4th (3 hits, the skip kept the count).
        assert(e.eval_tick(full, d.get()) == 4);   // len, iter, guard, act; io 1/3, caught false
        assert(e.rules()[0].consecutive_hits == 1);
        assert(e.eval_tick(bare, none.get()) == 2);   // act (fires, its action fails), caught
        assert(e.rules()[0].consecutive_hits == 1 && e.rules()[0].last_error.empty());
        assert(e.rules()[0].last_skip.find("disk is not collected at this level") != std::string::npos);
        assert(e.rules()[1].last_skip.find("]:4: cpu.cores is not collected at this level") != std::string::npos);
        assert(e.rules()[2].last_skip.find("]:5: tcp is not collected at this level") != std::string::npos);
        assert(e.rules()[3].last_skip.empty() && e.rules()[3].last_error.empty());
        assert(e.rules()[4].last_error.find("collected: unknown metric 'dsik'") != std::string::npos);
        assert(e.rules()[5].last_error.find("net is not collected at this level") != std::string::npos);
        std::vector<uint8_t> active;
        e.eval_level_conditions(bare, &active, none.get());
        assert((active == std::vector<uint8_t>{uint8_t(kFirstCustomLevel + 1)}));
        assert(e.level_conditions()[0].last_skip.find("disk is not collected at this level") !=
               std::string::npos);
        assert(e.level_conditions()[0].last_error.empty());
        e.eval_level_conditions(full, &active, d.get());
        assert(active.size() == 2);

        assert(e.eval_tick(full, d.get()) == 4);
        assert(e.rules()[0].consecutive_hits == 2);
        assert(e.eval_tick(full, d.get()) == 5);   // io fires
        assert(e.rules()[0].fire_count == 1);

        // A rate on its first read (s.baseline, d->baseline): skipped the
        // same way, sustain kept, but not logged; the error says so.
        Sample first = full;
        clear_groups(&first, kGroupDisk | kGroupTcp);
        first.baseline = kGroupDisk | kGroupTcp;
        std::unique_ptr<budyk_detail_c> fd(new budyk_detail_c());
        fd->collected = kDetailAll & ~kDetailCores;
        fd->baseline  = kDetailCores;
        LuaEngine f;
        assert(f.init(false) == 0);
        assert(f.load_string(
            "watch('io', { for_ticks = 2, when = function() return disk.read_bytes_per_sec > 100 end, cooldown = 0 })\n"
            "watch('len', { when = function() return #cpu.cores >= 0 end, cooldown = 0 })\n"
            "watch('why', { when = function()\n"
            "  local ok, err = pcall(function() return tcp.established end)\n"
            "  return not ok and err:find('tcp has no rate yet: its first read at this level') ~= nil\n"
            "end, cooldown = 0 })\n"
            "watch('none', { when = function() return not collected('disk') end, cooldown = 0 })\n"
            "watch('nocores', { when = function() return not collected('cpu.cores') end, cooldown = 0 })\n") == 0);
        assert(f.eval_tick(full, d.get()) == 1);    // len; io 1/2
        assert(f.eval_tick(first, fd.get()) == 3);  // why, none, nocores; io and len skipped
        assert(f.rules()[0].consecutive_hits == 1);
        assert(f.rules()[0].last_skip.empty() && f.rules()[0].last_error.empty());
        assert(f.rules()[1].last_skip.empty() && f.rules()[1].last_error.empty());
        assert(f.eval_tick(full, d.get()) == 2);    // len, and io: the first read kept its count
        assert(f.add_level_condition(kFirstCustomLevel, "disk.read_bytes_per_sec > 100") == 0);
        f.eval_level_conditions(first, &active, fd.get());
        assert(active.empty());
        assert(f.level_conditions()[0].last_skip.empty() && f.level_conditions()[0].last_error.empty());
        f.shutdown();

        // A list alone: cpu collected, cpu.cores not.
        d->collected = kDetailAll & ~kDetailCores;
        assert(e.eval_tick(full, d.get()) == 3);   // len skipped, guard still true
        assert(e.rules()[1].fire_count == 3);

        // Swapping the handlers back and forth allocates nothing that stays.
        for (int i = 0; i < 200; ++i) e.eval_tick(i % 2 ? full : bare, i % 2 ? d.get() : none.get());
        const size_t settled = e.memory_used();
        for (int i = 0; i < 2000; ++i) e.eval_tick(i % 2 ? full : bare, i % 2 ? d.get() : none.get());
        assert(e.memory_used() < settled + 64 * 1024);
        e.shutdown();
    }

    // N+3. when() results (spec §3.6.5): nil skips the tick and keeps the
    //      streak; no value (an `if` without an else) is false and resets
    //      it, like false.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "mode = 'true'\n"
            "local function pick()\n"
            "  if mode == 'true' then return true elseif mode == 'nil' then return nil\n"
            "  elseif mode == 'false' then return false end\n"
            "end\n"
            "watch('r', { for_ticks = 3, cooldown = 0, action = 'log', when = pick })\n") == 0);
        const Sample x = mk(1, 50, 0, 0);
        auto set = [&](const char* m) {
            assert(e.load_string((std::string("mode = '") + m + "'").c_str()) == 0);
        };
        assert(e.eval_tick(x) == 0 && e.rules()[0].consecutive_hits == 1);
        set("nil");
        assert(e.eval_tick(x) == 0 && e.rules()[0].consecutive_hits == 1);   // kept
        set("true");
        assert(e.eval_tick(x) == 0 && e.rules()[0].consecutive_hits == 2);
        set("none");                                                         // no value
        assert(e.eval_tick(x) == 0 && e.rules()[0].consecutive_hits == 0);
        set("true");
        e.eval_tick(x); e.eval_tick(x);
        set("false");
        assert(e.eval_tick(x) == 0 && e.rules()[0].consecutive_hits == 0);
        set("true");
        e.eval_tick(x); e.eval_tick(x);
        set("nil");
        e.eval_tick(x); e.eval_tick(x);
        set("true");
        assert(e.eval_tick(x) == 1 && e.rules()[0].fire_count == 1);       // 2 + nil + 1
        assert(e.rules()[0].last_error.empty());
        e.shutdown();
    }

    // N+4. An action list (spec §3.6.2) runs in order: the built-ins as
    //      values or names, functions, a failing function logged without
    //      stopping the rest. Every alert is queued for take_alerts() (and
    //      the WebSocket), as is one from alert() in Lua; past the cap
    //      they are counted, not kept.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        e.set_level_names({"L1", "L2", "L3"});
        assert(e.load_string(
            "order = {}\n"
            "watch('multi', { cooldown = 0, severity = 'critical', message = 'disk full',\n"
            "  when = function() return true end,\n"
            "  action = { function() order[#order + 1] = 'a' end, alert, 'log',\n"
            "             function() error('boom') end, escalate,\n"
            "             function() order[#order + 1] = 'b' end } })\n"
            "watch('esc', { cooldown = 0, when = function() return true end, action = escalate })\n"
            "watch('lua', { cooldown = 0, when = function() return true end,\n"
            "  action = function() alert('from_lua', 'info', 'hello') end })\n") == 0);
        assert(e.eval_tick(mk(1, 50, 0, 0)) == 3);
        assert(e.load_string("assert(table.concat(order, ',') == 'a,b')") == 0);
        assert(e.rules()[0].last_error.find("boom") != std::string::npos);
        const auto esc = e.take_escalations();
        assert(esc.size() == 2 && esc[0].level == "L3" && esc[0].seconds == 60);
        auto alerts = e.take_alerts();
        assert(alerts.size() == 2);
        assert(alerts[0].rule == "multi" && alerts[0].severity == "critical" &&
               alerts[0].message == "disk full" && alerts[0].ts_ns > 0);
        assert(alerts[1].rule == "from_lua" && alerts[1].severity == "info" &&
               alerts[1].message == "hello");
        assert(e.take_alerts().empty());
        assert(e.alerts().dispatch_calls() == 2);

        assert(e.load_string("for i = 1, 300 do alert('flood') end") == 0);
        assert(e.take_alerts().size() == LuaEngine::kMaxPendingAlerts);
        assert(e.alerts_dropped() == 300 - LuaEngine::kMaxPendingAlerts);
        e.shutdown();
    }

    // N+5. exec() written in the rules file runs nothing: it is checked
    //      (enabled, absolute, no traversal, allowlist) and returns the
    //      command as an action, alone or in a list, run (queued) when
    //      the rule fires and checked again then.
    if (access("/bin/sh", X_OK) == 0) {
        char tmpl[] = "/tmp/budyk_defer_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        assert(dir != nullptr);
        const std::string marker = std::string(dir) + "/ran";
        LuaEngine e;
        assert(e.init(/*enable_exec*/ true) == 0);
        const std::string rules =
            "loaded = exec({'/bin/sh', '-c', 'touch " + marker + ".load'})\n"
            "watch('fix', { cooldown = 0, when = function() return cpu.total_percent > 90 end,\n"
            "  action = { 'log', exec({'/bin/sh', '-c', 'touch " + marker + "'}) } })\n"
            "watch('one', { cooldown = 0, when = function() return cpu.total_percent > 90 end,\n"
            "  action = exec({'/bin/sh', '-c', 'touch " + marker + ".one'}) })\n"
            "assert(type(loaded) == 'function')\n";
        assert(e.load_string(rules.c_str()) == 0);
        assert(e.exec_worker().wait_idle(2000));
        assert(e.exec_worker().completed() == 0);
        assert(access((marker + ".load").c_str(), F_OK) != 0);   // nothing ran at load

        assert(e.eval_tick(mk(10, 50, 0, 0)) == 0);
        assert(e.eval_tick(mk(95, 50, 0, 0)) == 2);
        assert(e.exec_worker().wait_idle(5000));
        assert(e.exec_worker().completed() == 2);
        assert(access(marker.c_str(), F_OK) == 0 && access((marker + ".one").c_str(), F_OK) == 0);

        // Checked again when it runs: an allowlist set since makes it fail.
        e.set_exec_allowlist({"/bin/true"});
        assert(e.eval_tick(mk(95, 50, 0, 0)) == 2);
        assert(e.rules()[0].last_error.find("not in allowlist") != std::string::npos);
        assert(e.exec_worker().wait_idle(2000) && e.exec_worker().completed() == 2);

        // At load: a bad command fails the load, so does exec disabled.
        for (const char* bad : { "x = exec('sh')", "x = exec('/bin/../bin/sh')",
                                 "x = exec('/bin/sh')" }) {
            assert(e.load_string(bad) != 0);
        }
        assert(e.last_error().find("not in allowlist") != std::string::npos);
        e.shutdown();
        LuaEngine off;
        assert(off.init(/*enable_exec*/ false) == 0);
        assert(off.load_string(
            "watch('x', { when = function() return true end, action = exec('/bin/true') })") != 0);
        assert(off.last_error().find("exec is disabled") != std::string::npos);
        assert(off.rule_count() == 0);
        off.shutdown();
        for (const char* f : { "", ".one" }) ::unlink((marker + f).c_str());
        ::rmdir(dir);
    }

    // N+6. proc.top: the top processes as a read-only array (pid, name,
    //      state, threads, cpu_percent, rss_bytes); not collected without
    //      the list.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('hog', { cooldown = 0, action = 'log', when = function()\n"
            "  for _, p in ipairs(proc.top) do\n"
            "    if p.cpu_percent > 90 and p.name == 'stress' and p.state == 'R' then return true end\n"
            "  end\n"
            "  return false\n"
            "end })\n"
            "watch('fat', { cooldown = 0, action = 'log', when = function()\n"
            "  return #proc.top == 2 and proc.top[2].rss_bytes > 1e9 and proc.top[2].pid == 2\n"
            "    and proc.top[2].threads == 4\n"
            "end })\n"
            "watch('ro', { cooldown = 0, when = function() proc.top[1].pid = 0 return true end })\n") == 0);
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        d->collected = kDetailAll;
        d->nprocs = 2;
        d->procs[0] = budyk_process_c{ 1, "stress", 'R', 1, 99.5, 1000 };
        d->procs[1] = budyk_process_c{ 2, "db", 'S', 4, 0.5, 2ULL << 30 };
        const Sample x = mk(10, 50, 0, 0);
        assert(e.eval_tick(x, d.get()) == 2);
        assert(e.rules()[2].last_error.find("proc.top[1].pid is read-only") != std::string::npos);
        d->collected = kDetailAll & ~kDetailProcesses;
        assert(e.eval_tick(x, d.get()) == 0);
        assert(e.rules()[0].last_skip.find("proc.top is not collected at this level") != std::string::npos);
        e.shutdown();
    }

    // N+7. icmp: read-only like the other groups; not collected without
    //      the group.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('flood', { cooldown = 0, action = 'log', when = function()\n"
            "  return icmp.present and icmp.in_msgs_per_sec > 1000 and icmp.in_dest_unreachs_per_sec == 3\n"
            "end })\n") == 0);
        Sample x = mk(10, 50, 0, 0);
        x.icmp.present = true;
        x.icmp.in_msgs_per_sec = 5000;
        x.icmp.in_dest_unreachs_per_sec = 3;
        assert(e.eval_tick(x) == 1);
        clear_groups(&x, kGroupIcmp);
        assert(e.eval_tick(x) == 0);
        assert(e.rules()[0].last_skip.find("icmp is not collected at this level") != std::string::npos);
        e.shutdown();
    }

    // N+8. A "strict globals" guard on _G (a __newindex that raises for
    //      an undeclared global) does not trip the bind: the sample
    //      globals and `files` are raw-set. The bind used to call it
    //      outside any protected call: a Lua panic, the daemon aborted.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        FileWatchState fs;
        fs.modifies["/etc/passwd"] = 2;
        e.set_file_state(fs);
        assert(e.load_string(
            "watch('hot', { cooldown = 0, when = function()\n"
            "  return cpu.total_percent > 50 and files['/etc/passwd'].modifies == 2\n"
            "    and uptime_seconds > 0\n"
            "end })\n"
            "setmetatable(_G, { __newindex = function(_, k) error('undeclared global ' .. k) end })\n")
            == 0);
        assert(e.eval_tick(mk(90, 50, 0, 0)) == 1);
        assert(e.rules()[0].last_error.empty());
        // A rule's own assignment to a new global still meets the guard.
        assert(e.load_string("watch('w', { cooldown = 0, when = function() x = 1 return true end })")
               == 0);
        assert(e.eval_tick(mk(90, 50, 0, 0)) == 1);
        assert(e.rules()[1].last_error.find("undeclared global x") != std::string::npos);
        e.shutdown();
    }

    // N+9. A rule that holds the state at rules.limits.memory_mb makes
    //      the next bind's allocations fail: the tick goes without rules
    //      (logged), the daemon goes on, and the rules run again once
    //      there is room. That bind used to raise unprotected: a panic.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        e.set_limits(LuaEngine::kDefaultInstructionLimit, 2u << 20);
        assert(e.load_string(
            "watch('fill', { cooldown = 0, when = function()\n"
            "  if filled then return false end\n"
            "  filled = true\n"
            "  while true do hog = { hog } end\n"   // small tables, up to the last bytes
            "end })\n"
            "watch('ok', { cooldown = 0, when = function() return true end })\n") == 0);
        const Sample x = mk(10, 50, 0, 0);
        e.eval_tick(x);                                   // 'fill' fills the state
        assert(e.rules()[0].last_error.find("not enough memory") != std::string::npos);
        const uint64_t ok_fired = e.rules()[1].fire_count;

        // New processes: new item tables and names, so the bind allocates.
        std::unique_ptr<budyk_detail_c> d(new budyk_detail_c());
        d->collected = kDetailAll;
        d->nprocs = 2 * BUDYK_TOP_EACH;
        for (uint32_t i = 0; i < d->nprocs; ++i) {
            d->procs[i] = budyk_process_c{ static_cast<int32_t>(i + 1), "", 'S', 1, 0.0, 1 };
            std::snprintf(d->procs[i].name, sizeof(d->procs[i].name), "proc-%u", i);
        }
        assert(e.eval_tick(x, d.get()) == 0);
        assert(e.rules()[1].fire_count == ok_fired);      // nothing evaluated

        e.set_limits(LuaEngine::kDefaultInstructionLimit, 64u << 20);
        assert(e.eval_tick(x, d.get()) == 1);             // 'ok' again
        assert(e.rules()[1].fire_count == ok_fired + 1);
        e.shutdown();
    }

    // N+10. pairs() over a sample table hands out the proxy, not the data
    //       behind it: a rule cannot write through it, and the next rule
    //       reads the real value.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('writer', { cooldown = 0, when = function()\n"
            "  local _, t = pairs(cpu)\n"
            "  local ok = pcall(function() t.total_percent = 0 end)\n"
            "  return t == cpu and not ok\n"
            "end })\n"
            "watch('reader', { cooldown = 0, when = function()\n"
            "  local seen\n"
            "  for k, v in pairs(cpu) do if k == 'total_percent' then seen = v end end\n"
            "  return cpu.total_percent == 75 and seen == 75\n"
            "end })\n") == 0);
        assert(e.eval_tick(mk(75, 50, 0, 0)) == 2);
        e.shutdown();
    }

    // N+11. watch() reads each field of opts once: an __index that
    //       answers differently the second time (a table where a string
    //       was) used to crash the load, the message read again by C++.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "local seen = {}\n"
            "local fields = { message = 'first', when = function() return true end }\n"
            "watch('flip', setmetatable({ cooldown = 0 }, { __index = function(_, k)\n"
            "  if seen[k] then return {} end\n"
            "  seen[k] = true\n"
            "  return fields[k]\n"
            "end }))\n") == 0);
        assert(e.rule_count() == 1);
        assert(e.rules()[0].message == "first");
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        e.shutdown();
    }

    // N+12. save_state() that cannot write keeps the old file: a write
    //       that fails (a full disk: /dev/full) used to be renamed over
    //       it, and every cooldown reset on the next start.
    if (access("/dev/full", W_OK) == 0) {
        char tmpl[] = "/tmp/budyk_state_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        assert(dir != nullptr);
        const std::string path = std::string(dir) + "/state.tsv";
        const std::string tmp  = path + ".tmp";

        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string("watch('r', { for_ticks = 1, cooldown = 5, when = function() return true end })")
               == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);                // cooldown 5
        assert(e.save_state(path.c_str()) == 0);

        assert(::symlink("/dev/full", tmp.c_str()) == 0);        // the next write fails
        e.eval_tick(mk(0, 0, 0, 0));                             // cooldown 4
        assert(e.save_state(path.c_str()) == -ENOSPC);
        assert(access(tmp.c_str(), F_OK) != 0);                  // the temp file is gone

        LuaEngine back;                                          // the old file is intact
        assert(back.init(false) == 0);
        assert(back.load_string("watch('r', { for_ticks = 1, cooldown = 5, when = function() return true end })")
               == 0);
        assert(back.load_state(path.c_str()) == 0);
        assert(back.rules()[0].cooldown_remaining == 5);
        back.shutdown();
        e.shutdown();
        ::unlink(path.c_str());
        ::rmdir(dir);
    }

    // N+13. The instruction limit holds against what used to dodge it.
    //       Each case once never returned (the tick waited forever); the
    //       alarm turns a regression into a failure instead of a hang.
    ::alarm(120);
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        e.set_limits(200000, LuaEngine::kDefaultMemoryLimit);
        const char* runaway[] = {
            // pcall() / xpcall() catching the limit's error and going on
            "while true do pcall(function() while true do end end) end",
            "while true do xpcall(function() while true do end end, function() while true do end end) end",
            // a message handler looping on its own: Lua ran it with the
            // hooks off, the limit's error having come from the hook
            "xpcall(function() while true do end end, function() while true do end end)",
            "local ok = pcall(function() while true do end end) error({})",
            // backtracking patterns: one VM instruction, unbounded C
            "return string.find(string.rep('a', 4000), string.rep('a*', 12) .. 'b')",
            "return (string.rep('a', 4000)):match(string.rep('a-', 12) .. 'b')",
            "for _ in string.gmatch(string.rep('a', 4000), string.rep('a*', 12) .. 'b') do end",
            "return string.gsub(string.rep('a', 4000), string.rep('a*', 12) .. 'b', '')",
            "return string.find(string.rep('(', 4000), '%b()')",
            // a plain search with a long needle
            "return string.find(string.rep('a', 1000000), string.rep('a', 400000) .. 'b', 1, true)",
            // nil moved onto nil, a quintillion times
            "table.move({}, 1, math.maxinteger - 1, 2)",
        };
        std::string src;
        int i = 0;
        for (const char* body : runaway) {
            src += "watch('r" + std::to_string(i++) + "', { cooldown = 0, when = function() " +
                   body + " end })\n";
        }
        src += "watch('ok', { cooldown = 0, when = function() return true end })\n";
        assert(e.load_string(src.c_str()) == 0);
        const int n = static_cast<int>(sizeof(runaway) / sizeof(runaway[0]));
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);                  // only 'ok'
        for (int r = 0; r < n; ++r) {
            const std::string& err = e.rules()[static_cast<size_t>(r)].last_error;
            if (err.find("instruction limit of 200000 exceeded") == std::string::npos) {
                std::fprintf(stderr, "rule r%d: '%s'\n", r, err.c_str());
                assert(false);
            }
        }
        // The next tick starts with the hook back at its interval.
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);

        // A level condition is held to it the same way.
        assert(e.add_level_condition(4, "string.find(string.rep('a', 4000), "
                                        "string.rep('a*', 12) .. 'b') ~= nil") == 0);
        std::vector<uint8_t> active;
        e.eval_level_conditions(mk(0, 0, 0, 0), &active);
        assert(active.empty());
        assert(e.level_conditions()[0].last_error.find("instruction limit") != std::string::npos);
        e.shutdown();
    }

    // N+14. What the limit does not need to stop still works as Lua's:
    //       patterns, rep, move, xpcall and the table functions on plain
    //       tables. rep("") is "" at once; a table with __len, __index or
    //       __newindex is refused by concat / insert / remove / sort, and
    //       a metatable with __gc by setmetatable.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('lua', { cooldown = 0, when = function()\n"
            "  local k, v = ('key=value'):match('^(%w+)=(%w+)$')\n"
            "  local s, n = string.gsub('a,b,,c', ',', ';')\n"
            "  local up = ('hello world'):gsub('%w+', function(w) return w:upper() end)\n"
            "  local words = 0 for _ in ('one two  three'):gmatch('%S+') do words = words + 1 end\n"
            "  local i, j = string.find('x(a(b)c)y', '%b()')\n"
            "  local f = string.find('THE (quick) fox', '%f[%a]%a+', 5)\n"
            "  local p = string.find('a.b', '.', 1, true)\n"
            "  local t = { 3, 1, 2 } table.sort(t) table.insert(t, 4) table.remove(t, 1)\n"
            "  local m = table.move({ 1, 2, 3 }, 1, 3, 2, {})\n"
            "  return k == 'key' and v == 'value' and s == 'a;b;;c' and n == 3\n"
            "    and up == 'HELLO WORLD' and words == 3 and i == 2 and j == 8 and f == 6\n"
            "    and p == 2 and table.concat(t, ',') == '2,3,4' and m[2] == 1 and m[4] == 3\n"
            "    and string.rep('ab', 3, '-') == 'ab-ab-ab' and string.rep('', math.maxinteger) == ''\n"
            "    and string.rep('', 5, '') == '' and string.rep('x', 0) == ''\n"
            "    and select(2, xpcall(function(a, b) return a + b end, print, 2, 3)) == 5\n"
            "    and select(2, xpcall(function() error('x', 0) end, function(m) return m .. '!' end)) == 'x!'\n"
            "    and not xpcall(error, function() error('again') end)\n"
            "    and getmetatable(setmetatable({}, { __index = {} })) ~= nil\n"
            "end })\n"
            "watch('len', { cooldown = 0, when = function()\n"
            "  table.insert(setmetatable({}, { __len = function() return 1e15 end }), 1, 'x')\n"
            "end })\n"
            "watch('index', { cooldown = 0, when = function()\n"
            "  return table.concat(setmetatable({}, { __index = table.concat }), '', 1, 1e15)\n"
            "end })\n"
            // Lua runs finalizers with hooks off: no __gc in rules
            "watch('gc', { cooldown = 0, when = function()\n"
            "  setmetatable({}, { __gc = function() while true do end end })\n"
            "end })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        assert(e.rules()[0].last_error.empty());
        assert(e.rules()[1].last_error.find("table.insert: a table with __len is not supported") !=
               std::string::npos);
        assert(e.rules()[2].last_error.find("table.concat: a table with __index is not supported") !=
               std::string::npos);
        assert(e.rules()[3].last_error.find("setmetatable: __gc is not supported") != std::string::npos);
        e.shutdown();
    }
    ::alarm(0);

    std::printf("test_lua_engine: PASS\n");
    return 0;
}
