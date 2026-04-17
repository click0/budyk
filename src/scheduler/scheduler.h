// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

#include <atomic>
#include <cstdint>

namespace budyk {

enum class AnomalyState { NORMAL, ELEVATED, CRITICAL };

struct SchedulerConfig {
    int  l1_interval_sec   = 300;
    int  l2_interval_sec   = 30;
    int  l3_interval_sec   = 1;
    int  hysteresis_sec    = 300;
    int  grace_period_sec  = 60;
    bool l2_always_on      = false;

    double escalation_load_1m      = 4.0;
    double escalation_cpu_percent  = 85.0;
    double escalation_swap_percent = 50.0;
    double critical_load_1m        = 8.0;
    double critical_cpu_percent    = 95.0;
    double critical_swap_percent   = 80.0;
};

// 3-level adaptive scheduler (spec §3.4). The current sample's
// timestamp_nanos is the scheduler's clock — `tick()` is fully
// deterministic and unit-testable without wall-clock access.
class Scheduler {
public:
    explicit Scheduler(const SchedulerConfig& cfg);

    Level tick(const Sample& current);

    void client_connected();
    void client_disconnected();

    Level        current_level()   const;
    AnomalyState current_anomaly() const;
    int          client_count()    const;

private:
    SchedulerConfig  cfg_;
    Level            level_;
    AnomalyState     anomaly_;
    std::atomic<int> client_count_{0};
    uint64_t         last_anomaly_ns_;
    uint64_t         last_client_active_ns_;
    bool             had_anomaly_;
    bool             had_clients_;

    AnomalyState check_anomaly(const Sample& s) const;
};

} // namespace budyk
