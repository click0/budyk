// SPDX-License-Identifier: BSD-3-Clause
#pragma once
namespace budyk {

// Run the ncurses TUI against the running daemon on host:port. Blocks
// until the user quits (q / ESC) or the connection drops.
// host == nullptr → "127.0.0.1"; port <= 0 → 8080.
// Returns 0 on clean exit, negative on init / connection failure.
int tui_run(const char* host, int port);

} // namespace budyk
