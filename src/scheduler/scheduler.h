// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include <atomic>

namespace budyk {

enum class AnomalyState { NORMAL, ELEVATED, CRITICAL };

struct SchedulerConfig {
    int  l1_interval_sec = 300;
    int  l2_interval_sec = 30;
    int  l3_interval_sec = 1;
    int  hysteresis_sec  = 300;
    int  grace_period_sec = 60;
    bool l2_always_on    = false;

    double escalation_load_1m     = 4.0;
    double escalation_cpu_percent = 85.0;
    double escalation_swap_percent = 50.0;
};

class Scheduler {
public:
    explicit Scheduler(const SchedulerConfig& cfg);

    // Called after each collect tick — decide next level
    Level tick(const Sample& current);

    // WS hub calls these on client connect/disconnect
    void client_connected();
    void client_disconnected();

    Level current_level() const;

private:
    SchedulerConfig       cfg_;
    Level                 level_;
    AnomalyState          anomaly_;
    std::atomic<int>      client_count_{0};
    uint64_t              last_escalation_time_;
    uint64_t              last_client_disconnect_time_;

    AnomalyState check_anomaly(const Sample& s) const;
};

} // namespace budyk
