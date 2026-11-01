// SPDX-License-Identifier: BSD-3-Clause
#include "scheduler/scheduler.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace budyk;

static constexpr uint64_t kSec = 1'000'000'000ULL;

static Sample at(uint64_t ts_sec) {
    Sample s{};
    s.timestamp_nanos = ts_sec * kSec;
    s.level           = Level::L1;
    s.groups          = kGroupsAll;      // every reading present, as a collected sample's are
    return s;
}

int main() {
    SchedulerConfig cfg;  // defaults: hysteresis=300s, grace=60s

    // 1. Initial state is L1, no clients, no anomaly.
    {
        Scheduler sch(cfg);
        assert(sch.current_level() == Level::L1);
        assert(sch.client_count() == 0);
        assert(sch.tick(at(0))   == Level::L1);
        assert(sch.current_anomaly() == AnomalyState::NORMAL);
    }

    // 2. Client connect instantly escalates to L3. Disconnect enters grace,
    //    then de-escalates after grace expires.
    {
        Scheduler sch(cfg);
        sch.client_connected();
        assert(sch.tick(at(0))  == Level::L3);

        sch.client_disconnected();
        assert(sch.client_count() == 0);
        assert(sch.tick(at(30)) == Level::L3);   // within 60s grace
        assert(sch.tick(at(100)) == Level::L1);  // past grace
    }

    // 3. Anomaly escalates to L2. After it clears, L2 persists through
    //    hysteresis, then drops to L1.
    {
        Scheduler sch(cfg);
        Sample hot = at(10);
        hot.cpu.total_percent = 90.0;            // > escalation_cpu_percent=85
        assert(sch.tick(hot) == Level::L2);
        assert(sch.current_anomaly() == AnomalyState::ELEVATED);

        assert(sch.tick(at(20))  == Level::L2);  // within 300s hysteresis
        assert(sch.tick(at(400)) == Level::L1);  // past hysteresis
    }

    // 4. CRITICAL anomaly classified correctly.
    {
        Scheduler sch(cfg);
        Sample crit = at(0);
        crit.load.avg_1m = 10.0;                 // > critical_load_1m=8
        assert(sch.tick(crit) == Level::L2);
        assert(sch.current_anomaly() == AnomalyState::CRITICAL);
    }

    // 5. l2_always_on keeps the baseline at L2.
    {
        SchedulerConfig c2 = cfg;
        c2.l2_always_on = true;
        Scheduler sch(c2);
        assert(sch.tick(at(0)) == Level::L2);
    }

    // 6. Clients dominate anomaly.
    {
        Scheduler sch(cfg);
        sch.client_connected();
        Sample hot = at(0);
        hot.cpu.total_percent = 90.0;
        assert(sch.tick(hot) == Level::L3);
    }

    // 7. Multi-client refcount: still L3 until count hits 0.
    {
        Scheduler sch(cfg);
        sch.client_connected();
        sch.client_connected();
        assert(sch.client_count() == 2);
        sch.tick(at(0));

        sch.client_disconnected();
        assert(sch.client_count() == 1);
        assert(sch.tick(at(1)) == Level::L3);

        sch.client_disconnected();
        assert(sch.client_count() == 0);
        assert(sch.tick(at(30))  == Level::L3);  // within grace from tick@1s
        assert(sch.tick(at(100)) == Level::L1);
    }

    // 8. Anomaly ticks extend hysteresis each time.
    {
        Scheduler sch(cfg);
        Sample hot = at(0);
        hot.cpu.total_percent = 90.0;
        assert(sch.tick(hot) == Level::L2);

        hot.timestamp_nanos = 200 * kSec;
        assert(sch.tick(hot) == Level::L2);       // still anomalous

        assert(sch.tick(at(250)) == Level::L2);   // 250 - 200 = 50s < 300
        assert(sch.tick(at(600)) == Level::L1);   // 600 - 200 = 400s > 300
    }

    // 9. client_disconnected() guards against underflow.
    {
        Scheduler sch(cfg);
        sch.client_disconnected();
        sch.client_disconnected();
        assert(sch.client_count() == 0);
    }

    // 10. Client grace survives an intervening anomaly: clients take priority.
    {
        Scheduler sch(cfg);
        sch.client_connected();
        assert(sch.tick(at(0)) == Level::L3);
        sch.client_disconnected();
        Sample hot = at(10);
        hot.cpu.total_percent = 90.0;
        // In grace period — L3 wins over L2.
        assert(sch.tick(hot) == Level::L3);
    }

    // 8. set_client_count() — what the serve loop calls before every tick
    //    with the clients it can see. It replaces the count (no drift),
    //    drives L3, and dropping to 0 goes through grace back to L1.
    //    Negative input clamps to 0.
    {
        Scheduler sch(cfg);
        assert(sch.tick(at(0)) == Level::L1);
        sch.set_client_count(2);
        assert(sch.client_count() == 2);
        assert(sch.tick(at(1)) == Level::L3);
        sch.set_client_count(1);
        assert(sch.client_count() == 1);
        assert(sch.tick(at(2)) == Level::L3);
        sch.set_client_count(0);
        assert(sch.tick(at(30))  == Level::L3);   // within 60 s grace
        assert(sch.tick(at(100)) == Level::L1);
        sch.set_client_count(-3);
        assert(sch.client_count() == 0);
    }

    // 9. Custom levels: the active level with the highest priority wins,
    //    ties go to the shorter interval, requests expire at their
    //    deadline, and higher() / select() follow the same rule.
    {
        SchedulerConfig c = cfg;
        CustomLevel deep;  deep.id  = 4; deep.name  = "deep";  deep.interval_ms  = 1800000; deep.priority  = 5;
        CustomLevel burst; burst.id = 5; burst.name = "burst"; burst.interval_ms = 500;     burst.priority = 40;
        CustomLevel tie;   tie.id   = 6; tie.name   = "tie";   tie.interval_ms   = 200;     tie.priority   = 30;
        c.custom_levels = {deep, burst, tie};
        Scheduler sch(c);

        // Table lookups.
        assert(sch.interval_ms(Level::L1) == 300000);
        assert(sch.interval_ms(static_cast<Level>(5)) == 500);
        assert(sch.priority(static_cast<Level>(4)) == 5);
        assert(std::string(sch.level_name(static_cast<Level>(6))) == "tie");
        assert(std::string(sch.level_name(static_cast<Level>(9))) == "?");
        Level lv;
        assert(sch.level_by_name("l3", &lv) && lv == Level::L3);   // case-insensitive built-ins
        assert(sch.level_by_name("burst", &lv) && static_cast<int>(lv) == 5);
        assert(!sch.level_by_name("nope", &lv));

        // Nothing requested → L1.
        assert(sch.tick(at(0)) == Level::L1);

        // deep (5) beats L1 (0) while requested, then expires.
        sch.request(static_cast<Level>(4), 10 * kSec);
        assert(sch.tick(at(1))  == static_cast<Level>(4));
        assert(sch.tick(at(10)) == static_cast<Level>(4));          // deadline is inclusive
        assert(sch.tick(at(11)) == Level::L1);

        // An anomaly (L2, 20) beats deep (5).
        sch.request(static_cast<Level>(4), 100 * kSec);
        Sample hot = at(12); hot.cpu.total_percent = 90.0;
        assert(sch.tick(hot) == Level::L2);

        // burst (40) beats a connected client (L3, 30).
        sch.set_client_count(1);
        assert(sch.tick(at(13)) == Level::L3);
        sch.request_by_name("burst", 20 * kSec);
        assert(sch.tick(at(14)) == static_cast<Level>(5));

        // A later deadline extends, an earlier one doesn't shorten.
        sch.request(static_cast<Level>(5), 15 * kSec);
        assert(sch.tick(at(19)) == static_cast<Level>(5));
        assert(sch.tick(at(21)) == Level::L3);

        // Equal priority (tie 30 vs L3 30): the shorter interval wins.
        sch.request(static_cast<Level>(6), 30 * kSec);
        assert(sch.tick(at(22)) == static_cast<Level>(6));

        // Unknown names / ids are ignored.
        assert(!sch.request_by_name("nope", 99 * kSec));
        sch.request(static_cast<Level>(9), 99 * kSec);
        assert(sch.select(40 * kSec) != static_cast<Level>(9));

        // higher(): priority first, then interval.
        assert(sch.higher(Level::L1, static_cast<Level>(4)) == static_cast<Level>(4));
        assert(sch.higher(Level::L3, static_cast<Level>(6)) == static_cast<Level>(6));
        assert(sch.higher(static_cast<Level>(5), Level::L3) == static_cast<Level>(5));

        // select() reads state without changing the level tick() set.
        const Level before = sch.current_level();
        (void)sch.select(1000 * kSec);
        assert(sch.current_level() == before);
    }

    // 10b. The clock steps back a day with a request open: its deadline
    //      moves back as far as the clock did since the last tick, which
    //      had 50 s left of it, so it ends 50 s after that tick's time on
    //      the new clock (it held on for a day and 50 s). A request made
    //      after the step is as any other.
    {
        SchedulerConfig c = cfg;
        CustomLevel deep; deep.id = 4; deep.name = "deep"; deep.interval_ms = 1800000; deep.priority = 5;
        c.custom_levels = {deep};
        Scheduler sch(c);
        const uint64_t day = 86400;
        assert(sch.tick(at(2 * day)) == Level::L1);
        sch.request(static_cast<Level>(4), (2 * day + 60) * kSec);
        assert(sch.tick(at(2 * day + 10)) == static_cast<Level>(4));
        assert(sch.tick(at(day + 20)) == static_cast<Level>(4));      // stepped back: until day + 70
        assert(sch.tick(at(day + 69)) == static_cast<Level>(4));
        assert(sch.tick(at(day + 71)) == Level::L1);                  // not at 2 days + 60
        sch.request(static_cast<Level>(4), (day + 90) * kSec);
        assert(sch.tick(at(day + 80)) == static_cast<Level>(4));
        assert(sch.tick(at(day + 91)) == Level::L1);
    }

    // 10c. A request made on the new clock already (a custom level's
    //      `when` on the very sample the step back is seen on: the serve
    //      loop requests before it ticks) stays where it is. Moved back
    //      a day with the rest, it had ended before it began, and the
    //      level was missed for that tick (an L1 sleep of 5 minutes).
    {
        SchedulerConfig c = cfg;
        CustomLevel burst; burst.id = 4; burst.name = "burst"; burst.interval_ms = 500; burst.priority = 40;
        c.custom_levels = {burst};
        Scheduler sch(c);
        const uint64_t day = 86400;
        assert(sch.tick(at(2 * day)) == Level::L1);
        sch.request(static_cast<Level>(4), (day + 20 + 30) * kSec, (day + 20) * kSec);   // `when` held at day+20
        assert(sch.tick(at(day + 20)) == static_cast<Level>(4));      // the step back is seen here
        assert(sch.tick(at(day + 49)) == static_cast<Level>(4));
        assert(sch.tick(at(day + 51)) == Level::L1);
    }

    // 10d. The client grace and the L2 hysteresis move back with the
    //      clock as the requests do. Left where they were, each replayed
    //      its window a day later, when the clock reached the old stamp:
    //      60 s of L3 with nobody connected, 5 min of L2 with no anomaly.
    {
        Scheduler sch(cfg);
        const uint64_t day = 86400;
        sch.client_connected();
        assert(sch.tick(at(2 * day)) == Level::L3);
        sch.client_disconnected();
        assert(sch.tick(at(2 * day + 10)) == Level::L3);              // 10 s into the grace (from 2 days)
        assert(sch.tick(at(day + 20)) == Level::L3);                  // stepped back a day - 10: 10 s in
        assert(sch.tick(at(day + 69)) == Level::L3);                  // the grace ends on the new clock
        assert(sch.tick(at(day + 71)) == Level::L1);
        assert(sch.tick(at(2 * day + 30)) == Level::L1);              // the old stamp: no replay

        Scheduler h(cfg);
        Sample hot = at(2 * day); hot.cpu.total_percent = 90.0;
        assert(h.tick(hot) == Level::L2);
        assert(h.tick(at(2 * day + 10)) == Level::L2);                // hysteresis: 10 s in
        assert(h.tick(at(day + 20)) == Level::L2);                    // stepped back a day - 10: 10 s in
        assert(h.tick(at(day + 309)) == Level::L2);
        assert(h.tick(at(day + 311)) == Level::L1);
        assert(h.tick(at(2 * day + 30)) == Level::L1);                // the old stamp: no replay
    }

    // 11. A sustained anomaly escalates to L3 (spec §3.4.1). With
    //     escalate_after 100 s at the L2 cadence, ticks 0..90 stay L2 and
    //     the tick at 100 s goes L3. L3 then holds while the anomaly lasts
    //     and for grace_period (60 s) after its last tick; a dip inside
    //     the grace doesn't end it. Before escalation, one normal tick
    //     starts the count again; 0 turns escalation off.
    {
        SchedulerConfig c = cfg;
        c.escalate_after_sec = 100;
        auto hot = [](uint64_t t) { Sample x = at(t); x.cpu.total_percent = 90.0; return x; };

        Scheduler sch(c);
        for (uint64_t t = 0; t < 100; t += 30) {
            assert(sch.tick(hot(t)) == Level::L2);
            assert(!sch.escalated());
        }
        assert(sch.tick(hot(100)) == Level::L3 && sch.escalated());
        assert(sch.tick(hot(101)) == Level::L3);
        assert(sch.tick(at(120))  == Level::L3);   // a dip, 19 s after the last hot tick
        assert(sch.tick(hot(121)) == Level::L3);   // back: the same run
        assert(sch.tick(at(150))  == Level::L3);   // 29 s since 121
        assert(sch.tick(at(180))  == Level::L3);   // 59 s
        assert(sch.tick(at(182))  == Level::L2 && !sch.escalated());   // 61 s: over,
                                                   // hysteresis keeps L2
        assert(sch.tick(at(500))  == Level::L1);   // past hysteresis too

        // A normal tick before escalation resets the count.
        Scheduler b(c);
        assert(b.tick(hot(0))  == Level::L2);
        assert(b.tick(hot(90)) == Level::L2);
        assert(b.tick(at(95))  == Level::L2);      // normal: run broken
        assert(b.tick(hot(100)) == Level::L2);     // a new run starts at 100
        assert(b.tick(hot(190)) == Level::L2 && !b.escalated());
        assert(b.tick(hot(200)) == Level::L3 && b.escalated());

        // escalate_after 0: never.
        c.escalate_after_sec = 0;
        Scheduler z(c);
        for (uint64_t t = 0; t <= 3600; t += 30) assert(z.tick(hot(t)) == Level::L2);
        assert(!z.escalated());

        // A clock step back moves the run's start back as far rather
        // than escalating at once.
        c.escalate_after_sec = 100;
        Scheduler k(c);
        assert(k.tick(hot(1000)) == Level::L2);
        assert(k.tick(hot(10))   == Level::L2 && !k.escalated());
        assert(k.tick(hot(109))  == Level::L2);
        assert(k.tick(hot(110))  == Level::L3);
    }

    // 11b. grace_period 0, a value the config accepts: the escalation
    //      reaches L3 and ends on the first tick the anomaly is gone. The
    //      tick that set escalated_ cleared it again (0 >= 0), so L3 was
    //      never reached however long the anomaly held.
    {
        SchedulerConfig c = cfg;
        c.escalate_after_sec = 100;
        c.grace_period_sec   = 0;
        auto hot = [](uint64_t t) { Sample x = at(t); x.cpu.total_percent = 90.0; return x; };
        Scheduler sch(c);
        for (uint64_t t = 0; t < 100; t += 30) assert(sch.tick(hot(t)) == Level::L2);
        assert(sch.tick(hot(100)) == Level::L3 && sch.escalated());
        assert(sch.tick(hot(130)) == Level::L3 && sch.escalated());
        assert(sch.select(131 * kSec) == Level::L3);                  // between ticks too
        assert(sch.tick(at(160))  == Level::L2 && !sch.escalated());  // gone: over at once
    }

    // 11c. A tick without the CPU group (the collector failed, or the
    //      baseline tick after it came back) does not break the run: 0 %
    //      read as NORMAL did. A reading that is missing cannot say the
    //      anomaly is gone, so the state stays as it was when the rest
    //      is in bounds; one that is there and over a threshold still
    //      says so.
    {
        SchedulerConfig c = cfg;
        c.escalate_after_sec = 100;
        auto hot = [](uint64_t t) { Sample x = at(t); x.cpu.total_percent = 90.0; return x; };
        Scheduler sch(c);
        assert(sch.tick(hot(0))  == Level::L2);
        assert(sch.tick(hot(30)) == Level::L2);
        assert(sch.tick(hot(60)) == Level::L2);
        Sample blind = at(90);
        blind.groups &= ~static_cast<uint32_t>(kGroupCpu);           // cpu.total_percent is 0 here
        assert(sch.tick(blind) == Level::L2);
        assert(sch.tick(hot(100)) == Level::L3 && sch.escalated());  // the run from 0 held

        Scheduler n(cfg);
        Sample none = at(0);
        none.groups = 0;
        assert(n.tick(none) == Level::L1 && n.current_anomaly() == AnomalyState::NORMAL);
        assert(n.tick(hot(30)) == Level::L2 && n.current_anomaly() == AnomalyState::ELEVATED);
        none = at(60); none.groups = 0;
        assert(n.tick(none) == Level::L2 && n.current_anomaly() == AnomalyState::ELEVATED);
        Sample swapped = at(90);
        swapped.groups &= ~static_cast<uint32_t>(kGroupCpu);
        swapped.swap.used_percent = 90.0;                             // over, and there
        assert(n.tick(swapped) == Level::L2 && n.current_anomaly() == AnomalyState::CRITICAL);
        assert(n.tick(at(120)) == Level::L2 && n.current_anomaly() == AnomalyState::NORMAL);
    }

    // 12. collection.mode. always: L3 with no client, an anomaly or not,
    //     and a custom level of higher priority still wins. on-demand:
    //     thresholds raise nothing (no L2, no escalation, l2_always_on
    //     ignored), L1 and L2 collect nothing, a client still means L3 and
    //     a request still holds its level. adaptive collects at every level.
    {
        auto hot = [](uint64_t t) { Sample s = at(t); s.load.avg_1m = 99.0; return s; };
        SchedulerConfig c;
        c.mode = CollectionMode::Always;
        CustomLevel burst;
        burst.id = kFirstCustomLevel; burst.name = "burst"; burst.interval_ms = 100;
        burst.priority = 40;
        c.custom_levels.push_back(burst);
        Scheduler a(c);
        assert(a.mode() == CollectionMode::Always);
        assert(a.tick(at(0)) == Level::L3);
        assert(a.tick(hot(10)) == Level::L3);
        assert(a.tick(at(100000)) == Level::L3);
        assert(a.select(200000 * kSec) == Level::L3);
        a.request(static_cast<Level>(kFirstCustomLevel), 200001 * kSec);
        assert(a.select(200000 * kSec) == static_cast<Level>(kFirstCustomLevel));
        assert(a.collects(Level::L1) && a.collects(Level::L2) && a.collects(Level::L3));

        c.mode = CollectionMode::OnDemand;
        c.l2_always_on = true;
        c.escalate_after_sec = 10;
        Scheduler o(c);
        for (uint64_t t = 0; t <= 600; t += 30) assert(o.tick(hot(t)) == Level::L1);
        assert(!o.escalated() && o.current_anomaly() == AnomalyState::CRITICAL);
        assert(!o.collects(Level::L1) && !o.collects(Level::L2));
        assert(o.collects(Level::L3) && o.collects(static_cast<Level>(kFirstCustomLevel)));
        o.set_client_count(1);
        assert(o.tick(hot(630)) == Level::L3);
        o.set_client_count(0);
        assert(o.tick(at(640)) == Level::L3);                   // grace_period
        assert(o.tick(at(630 + 60)) == Level::L1);
        o.request(Level::L2, 800 * kSec);
        assert(o.tick(at(700)) == Level::L2 && !o.collects(Level::L2));
        o.request(Level::L3, 900 * kSec);
        assert(o.tick(at(850)) == Level::L3);
        assert(o.tick(at(901)) == Level::L1);

        c.mode = CollectionMode::Adaptive;
        Scheduler d(c);
        assert(d.mode() == CollectionMode::Adaptive);
        assert(d.tick(at(0)) == Level::L2);                     // l2_always_on
        for (int id = 1; id <= kMaxLevelId; ++id) assert(d.collects(static_cast<Level>(id)));
    }

    // 13. Each level's metric set: L1 standard, L2 / L3 / custom full by
    //     default; configured ones as given; an unknown level gets L1's.
    {
        SchedulerConfig c;
        Scheduler a(c);
        auto same = [](const MetricSet& x, const MetricSet& y) {
            return x.groups == y.groups && x.detail == y.detail;
        };
        assert(same(a.metrics(Level::L1), metric_set_standard()));
        assert(same(a.metrics(Level::L2), metric_set_full()));
        assert(same(a.metrics(Level::L3), metric_set_full()));

        c.l1_metrics = metric_set_minimal();
        c.l2_metrics = MetricSet{kGroupsMinimal | kGroupDisk, 0};
        c.l3_metrics = MetricSet{kGroupsMinimal, kDetailCores};
        CustomLevel lv;
        lv.id = kFirstCustomLevel + 1; lv.name = "x"; lv.interval_ms = 500; lv.priority = 5;
        lv.metrics = MetricSet{kGroupsMinimal | kGroupTcp, 0};
        c.custom_levels.push_back(lv);
        CustomLevel dflt;
        dflt.id = kFirstCustomLevel; dflt.name = "y"; dflt.interval_ms = 500; dflt.priority = 6;
        c.custom_levels.push_back(dflt);
        Scheduler b(c);
        assert(same(b.metrics(Level::L1), metric_set_minimal()));
        assert(same(b.metrics(Level::L2), MetricSet{kGroupsMinimal | kGroupDisk, 0}));
        assert(same(b.metrics(Level::L3), MetricSet{kGroupsMinimal, kDetailCores}));
        assert(same(b.metrics(static_cast<Level>(kFirstCustomLevel + 1)),
                    MetricSet{kGroupsMinimal | kGroupTcp, 0}));
        assert(same(b.metrics(static_cast<Level>(kFirstCustomLevel)), metric_set_full()));
        assert(same(b.metrics(static_cast<Level>(kMaxLevelId)), metric_set_minimal()));
    }

    std::printf("test_scheduler: PASS\n");
    return 0;
}
