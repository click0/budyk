// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "config/config.h"
#include "core/alert_event.h"
#include "hot_buffer/hot_buffer.h"
#include "scheduler/scheduler.h"
#include "storage/tier_manager.h"
#include "web/login_limiter.h"
#include "web/server.h"
#include "web/session.h"
#include "web/ws_hub.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

namespace budyk {

/// Everything the HTTP routes touch, handed over explicitly so the
/// router can be built and called in a unit test without a socket or a
/// daemon (tests/unit/test_router.cpp). The handler runs on the HTTP
/// thread: it reads `cfg` and `sched`'s fixed level table, takes
/// `hot_mtx` to snapshot the hot buffer, and calls into tm, sessions,
/// limiter and ws, which synchronise themselves.
struct RouterDeps {
    const Config*          cfg          = nullptr;
    HotBuffer*             hot          = nullptr;
    std::mutex*            hot_mtx      = nullptr;
    /// The newest sample's detail JSON (detail_to_json), under hot_mtx;
    /// /api/samples adds it. Optional.
    const std::string*     latest_detail = nullptr;
    /// The latest alerts (the last kRecentAlerts, oldest first), under
    /// hot_mtx; /api/alerts returns them. Optional.
    const std::deque<AlertEvent>* recent_alerts = nullptr;
    TierManager*           tm           = nullptr;
    SessionStore*          sessions     = nullptr;
    LoginLimiter*          limiter      = nullptr;
    WebSocketHub*          ws           = nullptr;
    const Scheduler*       sched        = nullptr;
    /// Realtime ns of the last authenticated GET /api/samples: a poller
    /// (the TUI) has no WebSocket, so a poll within poller_window_ns
    /// counts as one client for the scheduler.
    std::atomic<uint64_t>* last_poll_ns = nullptr;
    uint64_t               poller_window_ns = 5ULL * 1000000000ULL;
    /// Called when a client appears (a poller after a quiet spell, a
    /// WebSocket upgrade) so the collection loop steps up to L3 at once.
    std::function<void()>  wake;
    const char*            version      = "";
    /// This host's name (gethostname), in /api/health.
    std::string            hostname;
};

/// How many alerts the daemon keeps for /api/alerts (RAM only).
constexpr size_t kRecentAlerts = 100;

/// The daemon's HTTP routes: the SPA, /api/health, auth, samples, range,
/// levels, alerts and the WebSocket upgrade.
HttpHandler make_router(const RouterDeps& deps);

} // namespace budyk
