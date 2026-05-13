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
