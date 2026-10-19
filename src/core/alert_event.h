// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <string>

namespace budyk {

/// An alert a rule raised (spec §3.6.4: "alert: log + WS event"): what the
/// daemon logs, pushes to the dashboards and keeps for /api/alerts.
struct AlertEvent {
    uint64_t    ts_ns = 0;   ///< realtime, when it was raised
    std::string severity;    ///< "info" / "warning" / "critical"
    std::string rule;        ///< the rule's name, or alert()'s first argument
    std::string message;
};

} // namespace budyk
