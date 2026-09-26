// SPDX-License-Identifier: BSD-3-Clause
#include "scheduler/scheduler.h"

#include <strings.h>

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
    const uint64_t now = current.timestamp_nanos;

    anomaly_ = check_anomaly(current);
    if (anomaly_ != AnomalyState::NORMAL) {
        last_anomaly_ns_ = now;
        had_anomaly_     = true;
    }
    if (client_count_.load() > 0) {
        last_client_active_ns_ = now;
        had_clients_           = true;
    }

    level_ = select(now);
    return level_;
}

Level Scheduler::select(uint64_t now) const {
    const uint64_t hysteresis_ns = static_cast<uint64_t>(cfg_.hysteresis_sec)   * kNanosPerSec;
    const uint64_t grace_ns      = static_cast<uint64_t>(cfg_.grace_period_sec) * kNanosPerSec;

    auto requested = [&](Level lv) {
        const uint8_t id = static_cast<uint8_t>(lv);
        return requested_[id] && requested_until_ns_[id] >= now;
    };

    // L3: a client is connected, or the last one left less than
    // grace_period ago.
    const bool l3 = client_count_.load() > 0 ||
        (had_clients_ && now >= last_client_active_ns_ &&
         (now - last_client_active_ns_) < grace_ns) ||
        requested(Level::L3);

    // L2: a threshold is crossed now, or was less than hysteresis ago.
    const bool l2 = anomaly_ != AnomalyState::NORMAL ||
        (had_anomaly_ && now >= last_anomaly_ns_ &&
         (now - last_anomaly_ns_) < hysteresis_ns) ||
        cfg_.l2_always_on || requested(Level::L2);

    Level best       = Level::L1;
    int   best_prio  = kPriorityL1;
    int   best_ivl   = interval_ms(Level::L1);
    auto  consider = [&](Level lv, int prio) {
        const int ivl = interval_ms(lv);
        if (prio > best_prio || (prio == best_prio && ivl < best_ivl)) {
            best = lv; best_prio = prio; best_ivl = ivl;
        }
    };
    if (l2) consider(Level::L2, kPriorityL2);
    if (l3) consider(Level::L3, kPriorityL3);
    for (const auto& c : cfg_.custom_levels) {
        const Level lv = static_cast<Level>(c.id);
        if (requested(lv)) consider(lv, c.priority);
    }
    return best;
}

Level Scheduler::higher(Level a, Level b) const {
    const int pa = priority(a), pb = priority(b);
    if (pa != pb) return pa > pb ? a : b;
    return interval_ms(b) < interval_ms(a) ? b : a;
}

void Scheduler::request(Level level, uint64_t until_ns) {
    const uint8_t id = static_cast<uint8_t>(level);
    if (id < 1 || id > kMaxLevelId) return;
    if (id >= kFirstCustomLevel && find_custom(level) == nullptr) return;
    if (requested_[id] && requested_until_ns_[id] >= until_ns) return;
    requested_[id]          = true;
    requested_until_ns_[id] = until_ns;
}

bool Scheduler::request_by_name(const std::string& name, uint64_t until_ns) {
    Level lv;
    if (!level_by_name(name, &lv)) return false;
    request(lv, until_ns);
    return true;
}

const CustomLevel* Scheduler::find_custom(Level lv) const {
    const uint8_t id = static_cast<uint8_t>(lv);
    for (const auto& c : cfg_.custom_levels) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

int Scheduler::interval_ms(Level lv) const {
    switch (lv) {
        case Level::L1: return cfg_.l1_interval_sec * 1000;
        case Level::L2: return cfg_.l2_interval_sec * 1000;
        case Level::L3: return cfg_.l3_interval_sec * 1000;
    }
    const CustomLevel* c = find_custom(lv);
    return c != nullptr ? c->interval_ms : cfg_.l1_interval_sec * 1000;
}

int Scheduler::priority(Level lv) const {
    switch (lv) {
        case Level::L1: return kPriorityL1;
        case Level::L2: return kPriorityL2;
        case Level::L3: return kPriorityL3;
    }
    const CustomLevel* c = find_custom(lv);
    return c != nullptr ? c->priority : kPriorityL1;
}

const char* Scheduler::level_name(Level lv) const {
    switch (lv) {
        case Level::L1: return "L1";
        case Level::L2: return "L2";
        case Level::L3: return "L3";
    }
    const CustomLevel* c = find_custom(lv);
    return c != nullptr ? c->name.c_str() : "?";
}

bool Scheduler::level_by_name(const std::string& name, Level* out) const {
    if (out == nullptr) return false;
    if (::strcasecmp(name.c_str(), "L1") == 0) { *out = Level::L1; return true; }
    if (::strcasecmp(name.c_str(), "L2") == 0) { *out = Level::L2; return true; }
    if (::strcasecmp(name.c_str(), "L3") == 0) { *out = Level::L3; return true; }
    for (const auto& c : cfg_.custom_levels) {
        if (c.name == name) { *out = static_cast<Level>(c.id); return true; }
    }
    return false;
}

const std::vector<CustomLevel>& Scheduler::custom_levels() const {
    return cfg_.custom_levels;
}

void Scheduler::client_connected()    { ++client_count_; }
void Scheduler::client_disconnected() { if (client_count_.load() > 0) --client_count_; }
void Scheduler::set_client_count(int n) { client_count_.store(n > 0 ? n : 0); }

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
