// SPDX-License-Identifier: BSD-3-Clause
#include "scheduler/scheduler.h"

namespace budyk {

namespace {
constexpr uint64_t kNanosPerSec = 1'000'000'000ULL;
}

Scheduler::Scheduler(const SchedulerConfig& cfg)
    : cfg_(cfg),
      level_(Level::L1),
      anomaly_(AnomalyState::NORMAL),
      last_anomaly_ns_(0),
      last_client_active_ns_(0),
      had_anomaly_(false),
      had_clients_(false) {}

Level Scheduler::tick(const Sample& current) {
    const uint64_t now           = current.timestamp_nanos;
    const uint64_t hysteresis_ns = static_cast<uint64_t>(cfg_.hysteresis_sec)   * kNanosPerSec;
    const uint64_t grace_ns      = static_cast<uint64_t>(cfg_.grace_period_sec) * kNanosPerSec;

    anomaly_ = check_anomaly(current);
    if (anomaly_ != AnomalyState::NORMAL) {
        last_anomaly_ns_ = now;
        had_anomaly_     = true;
    }

    const int subs = client_count_.load();
    if (subs > 0) {
        last_client_active_ns_ = now;
        had_clients_           = true;
        level_                 = Level::L3;
        return level_;
    }

    // Client grace period — keep L3 briefly after the last client leaves.
    if (had_clients_ &&
        now >= last_client_active_ns_ &&
        (now - last_client_active_ns_) < grace_ns) {
        level_ = Level::L3;
        return level_;
    }

    // No clients, no grace. Anomaly escalates to L2 (instant).
    if (anomaly_ != AnomalyState::NORMAL) {
        level_ = Level::L2;
        return level_;
    }

    // Anomaly cleared — stay at L2 during hysteresis window.
    if (had_anomaly_ &&
        now >= last_anomaly_ns_ &&
        (now - last_anomaly_ns_) < hysteresis_ns) {
        level_ = Level::L2;
        return level_;
    }

    level_ = cfg_.l2_always_on ? Level::L2 : Level::L1;
    return level_;
}

void Scheduler::client_connected()    { ++client_count_; }
void Scheduler::client_disconnected() { if (client_count_.load() > 0) --client_count_; }

Level        Scheduler::current_level()   const { return level_; }
AnomalyState Scheduler::current_anomaly() const { return anomaly_; }
int          Scheduler::client_count()    const { return client_count_.load(); }

AnomalyState Scheduler::check_anomaly(const Sample& s) const {
    if (s.load.avg_1m       > cfg_.critical_load_1m       ) return AnomalyState::CRITICAL;
    if (s.cpu.total_percent > cfg_.critical_cpu_percent   ) return AnomalyState::CRITICAL;
    if (s.swap.used_percent > cfg_.critical_swap_percent  ) return AnomalyState::CRITICAL;
    if (s.load.avg_1m       > cfg_.escalation_load_1m     ) return AnomalyState::ELEVATED;
    if (s.cpu.total_percent > cfg_.escalation_cpu_percent ) return AnomalyState::ELEVATED;
    if (s.swap.used_percent > cfg_.escalation_swap_percent) return AnomalyState::ELEVATED;
    return AnomalyState::NORMAL;
}

} // namespace budyk
