// SPDX-License-Identifier: BSD-3-Clause
// budyk — lightweight FreeBSD server monitoring with adaptive collection
//
// Usage:
//   budyk serve [--config config.yaml]
//   budyk tui
//   budyk hash-password
//   budyk suggest-rules [--window 7d] [--ai --api-key KEY] [--output rules.lua]
//   budyk version

#include "web/auth.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <termios.h>
#include <unistd.h>

namespace {

// Read a line from stdin without echoing it. Falls back to a plain
// getline if stdin is not a tty (piped input — caller already knows
// the password is fine to be visible).
std::string read_password(const char* prompt) {
    std::fputs(prompt, stderr);
    std::fflush(stderr);

    const bool is_tty = ::isatty(STDIN_FILENO);
    struct termios saved;
    if (is_tty) {
        if (::tcgetattr(STDIN_FILENO, &saved) == 0) {
            struct termios noecho = saved;
            noecho.c_lflag &= ~ECHO;
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
        }
    }

    std::string line;
    std::getline(std::cin, line);

    if (is_tty) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        std::fputc('\n', stderr);
    }
    return line;
}

int cmd_hash_password() {
    const std::string p1 = read_password("Password: ");
    if (p1.empty()) {
        std::fprintf(stderr, "budyk hash-password: empty password\n");
        return 1;
    }

    // Skip confirmation when stdin is piped — the caller is presumably a
    // script, and asking for the same line twice would block forever.
    if (::isatty(STDIN_FILENO)) {
        const std::string p2 = read_password("Confirm:  ");
        if (p1 != p2) {
            std::fprintf(stderr, "budyk hash-password: passwords do not match\n");
            return 1;
        }
    }

    std::string encoded;
    const int rc = budyk::argon2_hash(p1, budyk::Argon2Params{}, &encoded);
    if (rc != 0) {
        std::fprintf(stderr, "budyk hash-password: argon2 hashing failed (rc=%d)\n", rc);
        return 1;
    }

    std::printf("%s\n", encoded.c_str());
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr,
            "budyk — lightweight server monitoring with adaptive collection\n"
            "\n"
            "Usage:\n"
            "  budyk serve           Start monitoring daemon + web server\n"
            "  budyk tui             Start terminal UI\n"
            "  budyk hash-password   Generate Argon2id password hash\n"
            "  budyk suggest-rules   Generate rule suggestions from history\n"
            "  budyk version         Show version\n"
        );
        return 1;
    }

    const char* cmd = argv[1];

    if (std::strcmp(cmd, "version") == 0) {
        std::printf("budyk 0.2.0\n");
        return 0;
    }

    if (std::strcmp(cmd, "serve") == 0) {
        // TODO: load config, init collector, scheduler, storage, rules, web
        std::fprintf(stderr, "budyk serve: not yet implemented\n");
        return 1;
    }

    if (std::strcmp(cmd, "tui") == 0) {
        // TODO: tui_run()
        std::fprintf(stderr, "budyk tui: not yet implemented\n");
        return 1;
    }

    if (std::strcmp(cmd, "hash-password") == 0) {
        return cmd_hash_password();
    }

    if (std::strcmp(cmd, "suggest-rules") == 0) {
        // TODO: parse --window, --ai, --output, run suggest
        std::fprintf(stderr, "budyk suggest-rules: not yet implemented\n");
        return 1;
    }

    std::fprintf(stderr, "budyk: unknown command '%s'\n", cmd);
    return 1;
}
