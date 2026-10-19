// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>
#include <vector>

namespace budyk {

/// Run the ncurses TUI against the running daemon on host:port. Blocks
/// until the user quits (q / ESC) or the connection drops.
/// host == nullptr → "127.0.0.1"; port <= 0 → 8080.
/// If the daemon requires a login (web.auth.enabled), ask_password is
/// called once, before the screen is taken over; nullptr means "don't ask"
/// and the TUI exits with an error instead.
/// Returns 0 on clean exit, negative on init / connection / login failure.
int tui_run(const char* host, int port, std::string (*ask_password)());

// Pieces of the TUI that don't need a terminal, exposed for tests.
namespace tui_detail {

struct HttpReply {
    int         status = 0;
    std::string headers;   ///< raw header block, CRLF-separated
    std::string body;
};

/// Splits a raw HTTP/1.x response. False if there is no status line or
/// no header/body separator.
bool parse_http_reply(const std::string& raw, HttpReply* out);

/// "budyk_session=<token>" from the Set-Cookie headers, or "" if none.
std::string session_cookie(const std::string& headers);

/// A JSON string literal (with quotes) for `s`.
std::string json_quote(const std::string& s);

/// The number at `section`.`key` in the newest sample of an /api/samples
/// body, e.g. ("mem", "total"). section == nullptr looks up a top-level
/// sample field such as "uptime_seconds". fallback if it isn't there.
double sample_number(const std::string& body, const char* section,
                     const char* key, double fallback = 0.0);

/// Whether the newest sample of an /api/samples body has `name` in its
/// list `key` ("groups", "baseline"): 1 yes, 0 no, -1 no such list.
int  sample_in_list(const std::string& body, const char* key, const char* name);

/// Whether the newest sample has metric group `group` ("cpu", "disk", ...).
/// A sample without a "groups" list (an older daemon) has every group.
bool sample_has_group(const std::string& body, const char* group);

/// One of the top processes in the "detail" of an /api/samples body.
struct ProcessRow {
    long        pid = 0;
    std::string name;
    std::string state;
    double      cpu_percent = 0.0;
    double      rss_bytes   = 0.0;
};

/// The "processes" list of an /api/samples body, in its order (largest
/// CPU first); names unescaped. Empty if there is none or it is cut off.
std::vector<ProcessRow> parse_processes(const std::string& body);

/// The "hostname" of a /api/health body, unescaped; "" if none.
std::string health_hostname(const std::string& body);

/// One row of GET /api/levels.
struct LevelInfo {
    int         id          = 0;
    std::string name;
    int         interval_ms = 0;
};

/// The level table from an /api/levels body; entries without an id or a
/// name are skipped.
std::vector<LevelInfo> parse_levels(const std::string& body);

/// "burst (every 0.5 s)", "L1 (every 5 min)", or "level 7" for an id the
/// table doesn't have. Same wording as the dashboard's Level row.
std::string level_label(const std::vector<LevelInfo>& levels, int id);

} // namespace tui_detail

} // namespace budyk
