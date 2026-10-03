// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rules/worker.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace budyk {

// Severity levels recognised by the alert dispatcher. The values are
// chosen so a serialised int round-trips cleanly with config / Lua.
enum class AlertSeverity : int {
    Info     = 0,
    Warning  = 1,
    Critical = 2,
};

const char* severity_name(AlertSeverity s);

// Parses "info" / "warning" / "critical" (any case). Returns false and
// leaves *out untouched for anything else.
bool parse_severity(const char* s, AlertSeverity* out);

// One configured destination — ntfy.sh / Discord / Telegram / SMTP /
// Twilio. The `type` field selects the dispatcher backend; the other
// fields are interpreted per backend:
//
//   ntfy:     url = base (https://ntfy.sh), topic = topic name
//   discord:  url = full webhook URL
//   telegram: token = bot token, topic = chat_id
//             (url optional override; default api.telegram.org)
//   smtp:     url = smtp[s]://host:port, token = "user:pass" (basic auth,
//             may be empty for unauthenticated relays), from = sender
//             address, topic = recipient address
//   twilio:   url = full Messages.json endpoint (contains Account SID),
//             token = "AccountSID:AuthToken", from = sender phone,
//             topic = recipient phone (E.164)
struct AlertChannel {
    std::string name;
    std::string type;
    std::string url;
    std::string topic;
    std::string token;
    std::string from;
};

// Lua-facing dispatcher. dispatch() queues the alert and returns at
// once; the sends — one curl run per channel, up to 20 s each — happen
// on the dispatcher's own thread (a Worker), so a channel that is slow
// or down never holds the collector tick. Delivery is best-effort: a
// failed channel is logged and the others still get the alert.
class AlertDispatcher {
public:
    void   add_channel(AlertChannel ch);

    // Queue the alert for every channel whose fields are complete (an
    // incomplete or unknown channel is logged and skipped, as before).
    // Returns the number of channels it was queued for; 0 when there
    // are none, or when the queue is full and the alert was dropped.
    int    dispatch(AlertSeverity sev,
                    const std::string& rule_name,
                    const std::string& message);

    // Block until every queued alert has been attempted, or timeout_ms
    // passed; true when the queue ran dry. Tests, and shutdown.
    bool   flush(int timeout_ms);
    // Wait up to grace_ms for queued alerts to go out, then cancel the
    // running curl and drop the rest. The daemon calls this on stop.
    void   stop(int grace_ms);

    size_t channel_count() const;

    // Bookkeeping over every dispatch() call, counted whether or not any
    // channel is configured or succeeds, so callers (and tests) can see
    // what fired without a network round-trip.
    struct Event {
        AlertSeverity severity = AlertSeverity::Warning;
        std::string   rule;
        std::string   message;
    };
    uint64_t     dispatch_calls() const;
    const Event& last_event()     const;
    // Outcome counters, per channel attempt, updated by the worker.
    uint64_t     delivered()      const;
    uint64_t     failed()         const;
    // Alerts dropped because the queue was full or the worker stopped.
    uint64_t     dropped()        const;

private:
    std::vector<AlertChannel> channels_;
    uint64_t                  dispatch_calls_ = 0;
    Event                     last_event_;
    Worker                    worker_{"alerts"};
    std::atomic<uint64_t>     delivered_{0};
    std::atomic<uint64_t>     failed_{0};
};

// --- Payload builders (exported for tests; no network) ---------------
std::string ntfy_payload    (AlertSeverity sev, const std::string& rule_name,
                             const std::string& message);
std::string discord_payload (AlertSeverity sev, const std::string& rule_name,
                             const std::string& message);
std::string telegram_payload(AlertSeverity sev, const std::string& chat_id,
                             const std::string& rule_name,
                             const std::string& message);
// SMTP DATA section — full RFC 5322-ish blob: From/To/Subject/Date
// headers, blank line, then the body. No CR-LF folding, no MIME.
std::string smtp_message    (AlertSeverity sev, const std::string& from,
                             const std::string& to,
                             const std::string& rule_name,
                             const std::string& message);
// Twilio /Messages.json POST body — application/x-www-form-urlencoded
// with three fields: From, To, Body.
std::string twilio_form     (const std::string& from, const std::string& to,
                             const std::string& rule_name,
                             const std::string& message);

} // namespace budyk
