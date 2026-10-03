// SPDX-License-Identifier: BSD-3-Clause
// budyk — lightweight server monitoring with adaptive collection
//
// Usage:
//   budyk serve [--config config.yaml] [--enable-exec] [--enable-freeze]
//   budyk tui [--host H] [--port P]
//   budyk hash-password
//   budyk suggest-rules [--config PATH] [--window 7d] [--output rules.lua] [--ai --api-key KEY]
//   budyk watch-files [--timeout MS] <path> [<path>...]
//   budyk version
//
// This file only dispatches. The daemon is src/daemon/ (serve loop,
// routes, collector bridge); the one-shot commands are src/cli/.

#include "cli/commands.h"
#include "daemon/serve.h"

#include <cstdio>
#include <cstring>

// Set by src/CMakeLists.txt from project(VERSION). The fallback keeps
// a build that bypasses CMake (and cppcheck, which sees no -D) going.
#ifndef BUDYK_VERSION
#define BUDYK_VERSION "0.0.0-unknown"
#endif

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
            "  budyk watch-files     Print file change events (diagnostic)\n"
            "  budyk version         Show version\n"
        );
        return 1;
    }

    const char* cmd = argv[1];

    if (std::strcmp(cmd, "version") == 0) {
        std::printf("budyk " BUDYK_VERSION "\n");
        return 0;
    }
    if (std::strcmp(cmd, "serve") == 0)         return budyk::cmd_serve(argc, argv, BUDYK_VERSION);
    if (std::strcmp(cmd, "tui") == 0)           return budyk::cmd_tui(argc, argv);
    if (std::strcmp(cmd, "hash-password") == 0) return budyk::cmd_hash_password();
    if (std::strcmp(cmd, "suggest-rules") == 0) return budyk::cmd_suggest_rules(argc, argv);
    if (std::strcmp(cmd, "watch-files") == 0)   return budyk::cmd_watch_files(argc, argv);

    std::fprintf(stderr, "budyk: unknown command '%s'\n", cmd);
    return 1;
}
