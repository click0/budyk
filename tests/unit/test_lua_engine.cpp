// SPDX-License-Identifier: BSD-3-Clause
#include "core/sample.h"
#include "rules/lua_engine.h"

#include <cassert>
#include <cstdio>
#include <cstring>

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

    std::printf("test_lua_engine: PASS\n");
    return 0;
}
