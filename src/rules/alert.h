// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
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

// One configured destination — Telegram / Discord / ntfy.sh / SMTP /
// Twilio. The `type` field selects the dispatcher backend, `url`
// carries the endpoint, `topic` and `token` are channel-specific
// (ntfy topic, Telegram chat id, SMTP recipient, etc.).
struct AlertChannel {
    std::string name;
    std::string type;
    std::string url;
    std::string topic;
    std::string token;
};

// Lua-facing dispatcher. Each call to dispatch() fires one HTTP POST
// per channel, best-effort: a failed channel logs to stderr and keeps
// the others alive.
class AlertDispatcher {
public:
    void   add_channel(AlertChannel ch);

    // Returns the number of channels that succeeded.
    int    dispatch(AlertSeverity sev,
                    const std::string& rule_name,
                    const std::string& message);

    size_t channel_count() const;

private:
    std::vector<AlertChannel> channels_;
};

// --- Payload builders (exported for tests; no network) ---------------
std::string ntfy_payload   (AlertSeverity sev, const std::string& rule_name,
                            const std::string& message);
std::string discord_payload(AlertSeverity sev, const std::string& rule_name,
                            const std::string& message);

} // namespace budyk
