// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace budyk {

enum class AnomalyState { NORMAL, ELEVATED, CRITICAL };

// A user-defined collection level (collection.levels in the config).
struct CustomLevel {
    uint8_t     id          = 0;   // kFirstCustomLevel + index, set by the loader
    std::string name;
    int         interval_ms = 0;   // gap between ticks while this level runs
    int         priority    = 0;   // highest active priority wins
    std::string when;              // Lua expression; empty = only escalate()
    int         hold_sec    = 0;   // stay active this long after `when` was true
    int         storage_mb  = 50;  // size of the level's own ring file
};

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

    std::vector<CustomLevel> custom_levels;
};

// Adaptive level scheduler (spec §3.4). The current sample's
// timestamp_nanos is the scheduler's clock — `tick()` is fully
// deterministic and unit-testable without wall-clock access.
//
// Each level is active or not; the active level with the highest
// priority wins, ties going to the shorter interval:
//   L1  priority  0   always active (the fallback)
//   L2  priority 20   a threshold is crossed, then for `hysteresis`
//   L3  priority 30   a client is connected, then for `grace_period`
//   custom levels     their configured priority, active while requested
// Any level can also be requested until a deadline (a custom level's
// `when` condition, or escalate() from a rule).
class Scheduler {
public:
    static constexpr int kPriorityL1 = 0;
    static constexpr int kPriorityL2 = 20;
    static constexpr int kPriorityL3 = 30;

    explicit Scheduler(const SchedulerConfig& cfg);

    // Updates the anomaly / client state from `current` and returns the
    // level selected for it.
    Level tick(const Sample& current);

    // The level that would be selected at `now_ns` from the current state
    // and requests, without taking a new sample. The serve loop uses it
    // after the rules have run, so an escalate() shortens the next sleep.
    Level select(uint64_t now_ns) const;

    // The level that wins between `a` and `b`: higher priority, then the
    // shorter interval. The serve loop sleeps at higher(sample level,
    // select(now)) — the sample's level holds until the next tick
    // re-checks it, so a `when` with hold 0 isn't dropped before the sleep.
    Level higher(Level a, Level b) const;

    // Keep `level` active through `until_ns` (a later deadline replaces an
    // earlier one; an earlier one is ignored). Unknown ids are ignored.
    void request(Level level, uint64_t until_ns);
    // Same, by level name ("L1".."L3" or a custom name). False if unknown.
    bool request_by_name(const std::string& name, uint64_t until_ns);

    // Level table lookups. Unknown ids fall back to L1's values / "?".
    int         interval_ms(Level lv) const;
    int         priority(Level lv)    const;
    const char* level_name(Level lv)  const;
    // Level for a name ("L1".."L3", case-insensitive, or a custom name);
    // returns false if there is none.
    bool        level_by_name(const std::string& name, Level* out) const;
    const std::vector<CustomLevel>& custom_levels() const;

    void client_connected();
    void client_disconnected();
    // Replaces the count outright. The serve loop calls this before every
    // tick with the number of clients it can actually see, so the count
    // can't drift the way paired connected/disconnected calls can.
    void set_client_count(int n);

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
    bool             requested_[kMaxLevelId + 1]          = {};
    uint64_t         requested_until_ns_[kMaxLevelId + 1] = {};

    AnomalyState       check_anomaly(const Sample& s) const;
    const CustomLevel* find_custom(Level lv) const;
};

} // namespace budyk
