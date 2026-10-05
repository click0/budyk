// SPDX-License-Identifier: BSD-3-Clause
#include "core/sample.h"
#include "rules/lua_engine.h"
#include "rules/alert.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

using namespace budyk;

static Sample mk(double cpu_pct, double mem_avail_pct, double load1, double swap_used_pct) {
    Sample s{};
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
            "  action = function() counter = counter + 1 end\n"
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

    // 11. for_ticks=0 is clamped to 1 (backwards-compatible with existing rules).
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('default', { when = function() return true end, for_ticks = 0 })\n") == 0);
        assert(e.rules()[0].for_ticks == 1);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        e.shutdown();
    }

    // 11b. exec(cmd, { wait = true }) — Lua gets a result table for
    //      /bin/true / /bin/false.
    //      Engine is initialised with enable_exec=true; the rule stashes the
    //      returned table in a global that a second rule inspects.
    {
        LuaEngine e;
        assert(e.init(/*enable_exec*/ true) == 0);
        const bool have_true  = access("/bin/true",  X_OK) == 0;
        const bool have_false = access("/bin/false", X_OK) == 0;

        if (have_true) {
            assert(e.load_string(
                "r_true = exec('/bin/true', { timeout = 5, wait = true })\n"
                "watch('true_ok', { when = function()\n"
                "  return r_true and r_true.ok == true\n"
                "           and r_true.exit_status == 0\n"
                "           and r_true.timed_out == false\n"
                "end })\n") == 0);
            assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        }
        if (have_false) {
            assert(e.load_string(
                "r_false = exec('/bin/false', { timeout = 5, wait = true })\n"
                "watch('false_ok', { when = function()\n"
                "  return r_false and r_false.exit_status == 1\n"
                "           and r_false.ok == false\n"
                "end })\n") == 0);
            // Previous 'true_ok' rule may still be registered — eval just
            // ensures the new rule fires; count ≥ 1.
            assert(e.eval_tick(mk(0, 0, 0, 0)) >= 1);
        }
        e.shutdown();
    }

    // 11c. exec() with an argv table — exec({'/bin/sh', '-c', 'exit 7'}, 5).
    {
        if (access("/bin/sh", X_OK) == 0) {
            LuaEngine e;
            assert(e.init(/*enable_exec*/ true) == 0);
            assert(e.load_string(
                "r = exec({'/bin/sh', '-c', 'exit 7'}, { timeout = 5, wait = true })\n"
                "watch('sh7', { when = function()\n"
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
            "r = exec('/bin/true', { timeout = 5, wait = true })\n"
            "watch('allowed', { when = function()\n"
            "  return r and r.ok == true and r.exit_status == 0\n"
            "end })\n") == 0);
        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);
        e.shutdown();
    }

    // 12. Disk + net bindings — rules reference `disk.*` and `net.*` tables.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        const char* script =
            "watch('disk_hot', { when = function() return disk.read_bytes_per_sec > 10000000 end })\n"
            "watch('net_hot',  { when = function() return net.tx_bytes_per_sec  > 1000000  end })\n"
            "watch('iface_ok', { when = function() return net.interface_count   >= 2       end })\n"
            "watch('devs_ok',  { when = function() return disk.device_count     >= 2       end })\n";
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
    //     nothing: an action table (the old `{ alert, escalate }` form),
    //     an unknown action string, an unknown severity, a non-string
    //     message. None of them registers a rule.
    {
        const char* bad[] = {
            "watch('t', { when = function() return true end, action = { alert } })",
            "watch('t', { when = function() return true end, action = 'page' })",
            "watch('t', { when = function() return true end, severity = 'high' })",
            "watch('t', { when = function() return true end, message = 42 })",
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

    // 29. severity is case-insensitive; cooldown defaults to 0, so a
    //     for_ticks = 1 rule fires on every hot tick.
    {
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(
            "watch('i', { when = function() return true end,"
            "             severity = 'INFO' })\n") == 0);
        assert(e.rules()[0].cooldown_ticks == 0);
        assert(e.eval_tick(mk(0, 50, 0, 0)) == 1);
        assert(e.eval_tick(mk(0, 50, 0, 0)) == 1);
        assert(e.alerts().dispatch_calls() == 2);
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
            "             action = function() escalate('BURST', 5) end })\n"
            "watch('b', { when = function() return true end,"
            "             action = function() escalate('L3') end })\n"
            "watch('c', { when = function() return true end,"
            "             action = function() escalate('nope', 5) end })\n"
            "watch('d', { when = function() return true end,"
            "             action = function() escalate('burst', 0) end })\n") == 0);
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
            "watch('spin', { when = function() while true do end end })\n"
            "watch('ok',   { when = function() return cpu.total_percent > 50 end })\n") == 0);
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
            " action = function() local n = 0 while true do n = n + 1 end end })\n") == 0);
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
            "end })\n"
            "watch('ok', { when = function() return true end })\n") == 0);

        assert(e.eval_tick(mk(0, 0, 0, 0)) == 1);   // 'ok' fires, 'hog' errors
        assert(e.rules()[0].last_error.find("not enough memory") != std::string::npos);
        assert(e.memory_used() <= (2u << 20));

        // One huge string from a single C call: no VM instructions to
        // count, so this is the memory limit's job alone.
        assert(e.load_string(
            "watch('big', { when = function() local s = ('x'):rep(64 * 1024 * 1024) return true end })\n") == 0);
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
            "end })\n";
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
            "r2 = exec({'/bin/sh', '-c', 'sleep 5'}, { timeout = 1, wait = true })\n"
            "r3 = exec('/bin/true', 5)\n"
            "watch('bg2', { when = function()\n"
            "  return r2.timed_out == true and r2.ok == false and r3.queued == true\n"
            "end })\n") == 0);
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
                "', { timeout = 5, wait = true }) return r.ok == true end })\n";
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
            "watch('writer', { when = function() cpu.total_percent = 0 return true end })\n"
            "watch('reader', { when = function() return cpu.total_percent > 50 end })\n"
            "watch('same', { when = function()\n"
            "  tick = tick + 1\n"
            "  if tick == 1 then first = cpu return false end\n"
            "  return rawequal(cpu, first) and cpu.total_percent == 20\n"
            "end })\n"
            "watch('pairs', { when = function()\n"
            "  local n = 0\n"
            "  for k, v in pairs(cpu) do n = n + 1 end\n"
            "  return n == 2\n"
            "end })\n"
            "watch('meta', { when = function()\n"
            "  local ok = pcall(setmetatable, cpu, nil)\n"
            "  return getmetatable(cpu) == 'read-only' and not ok\n"
            "end })\n") == 0);

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
            "end })\n"
            "watch('see', { when = function()\n"
            "  seen_cpu = cpu.total_percent\n"
            "  seen_mem = mem and mem.available_percent\n"
            "  return true\n"
            "end })\n") == 0);
        // Tick 1: the bypasses work within the tick...
        assert(e.load_string("seen_cpu = nil seen_mem = nil") == 0);
        assert(e.eval_tick(mk(90.0, 25.0, 0.1, 0.0)) == 1);
        assert(e.load_string("assert(seen_cpu == -1 and seen_mem == nil)") == 0);
        // ...and are gone on the next one.
        assert(e.eval_tick(mk(70.0, 30.0, 0.1, 0.0)) == 1);
        assert(e.load_string("assert(seen_cpu == 70 and seen_mem == 30)") == 0);
        e.shutdown();
    }

    std::printf("test_lua_engine: PASS\n");
    return 0;
}
