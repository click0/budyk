// SPDX-License-Identifier: BSD-3-Clause
#include "scheduler/scheduler.h"

namespace budyk {

Scheduler::Scheduler(const SchedulerConfig& cfg)
    : cfg_(cfg), level_(Level::L1), anomaly_(AnomalyState::NORMAL),
      last_escalation_time_(0), last_client_disconnect_time_(0) {}

Level Scheduler::tick(const Sample& current) {
    // TODO: implement L1↔L2↔L3 transitions
    // 1. If client_count_ > 0 → L3
    // 2. If check_anomaly() != NORMAL → L2 or L3
    // 3. Otherwise → L1
    // Apply hysteresis on de-escalation
    (void)current;
    return level_;
}

void Scheduler::client_connected()    { ++client_count_; }
void Scheduler::client_disconnected() { --client_count_; }
Level Scheduler::current_level() const { return level_; }

AnomalyState Scheduler::check_anomaly(const Sample& s) const {
    if (s.load.avg_1m > cfg_.escalation_load_1m)     return AnomalyState::ELEVATED;
    if (s.cpu.total_percent > cfg_.escalation_cpu_percent) return AnomalyState::ELEVATED;
    if (s.swap.used_percent > cfg_.escalation_swap_percent) return AnomalyState::ELEVATED;
    return AnomalyState::NORMAL;
}

} // namespace budyk
