// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>

namespace budyk {

// The one-shot subcommands. Each returns the process exit code.
int cmd_hash_password();                         // budyk hash-password
int cmd_suggest_rules(int argc, char* argv[]);   // budyk suggest-rules [...]
int cmd_watch_files(int argc, char* argv[]);     // budyk watch-files [...]
int cmd_tui(int argc, char* argv[]);             // budyk tui [--host H] [--port P]

// Read a line from stdin without echoing it (a plain getline when stdin
// is not a tty). Shared by hash-password and the TUI's login prompt.
std::string read_password(const char* prompt);

} // namespace budyk
