// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>

namespace budyk {

// Run the ncurses TUI against the running daemon on host:port. Blocks
// until the user quits (q / ESC) or the connection drops.
// host == nullptr → "127.0.0.1"; port <= 0 → 8080.
// If the daemon requires a login (web.auth.enabled), ask_password is
// called once, before the screen is taken over; nullptr means "don't ask"
// and the TUI exits with an error instead.
// Returns 0 on clean exit, negative on init / connection / login failure.
int tui_run(const char* host, int port, std::string (*ask_password)());

// Pieces of the TUI that don't need a terminal, exposed for tests.
namespace tui_detail {

struct HttpReply {
    int         status = 0;
    std::string headers;   // raw header block, CRLF-separated
    std::string body;
};

// Splits a raw HTTP/1.x response. False if there is no status line or
// no header/body separator.
bool parse_http_reply(const std::string& raw, HttpReply* out);

// "budyk_session=<token>" from the Set-Cookie headers, or "" if none.
std::string session_cookie(const std::string& headers);

// A JSON string literal (with quotes) for `s`.
std::string json_quote(const std::string& s);

// The number at `section`.`key` in the newest sample of an /api/samples
// body, e.g. ("mem", "total"). section == nullptr looks up a top-level
// sample field such as "uptime_seconds". fallback if it isn't there.
double sample_number(const std::string& body, const char* section,
                     const char* key, double fallback = 0.0);

} // namespace tui_detail

} // namespace budyk
