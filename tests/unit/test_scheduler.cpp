// SPDX-License-Identifier: BSD-3-Clause
#include "scheduler/scheduler.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace budyk;

static constexpr uint64_t kSec = 1'000'000'000ULL;

static Sample at(uint64_t ts_sec) {
    Sample s{};
    s.timestamp_nanos = ts_sec * kSec;
    s.level           = Level::L1;
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

    std::printf("test_scheduler: PASS\n");
    return 0;
}
