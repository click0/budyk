// SPDX-License-Identifier: BSD-3-Clause
// budyk — lightweight FreeBSD server monitoring with adaptive collection
//
// Usage:
//   budyk serve [--config config.yaml]
//   budyk tui
//   budyk hash-password
//   budyk suggest-rules [--window 7d] [--ai --api-key KEY] [--output rules.lua]
//   budyk version

#include <cstdio>
#include <cstring>

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
        // TODO: argon2id hash
        std::fprintf(stderr, "budyk hash-password: not yet implemented\n");
        return 1;
    }

    if (std::strcmp(cmd, "suggest-rules") == 0) {
        // TODO: parse --window, --ai, --output, run suggest
        std::fprintf(stderr, "budyk suggest-rules: not yet implemented\n");
        return 1;
    }

    std::fprintf(stderr, "budyk: unknown command '%s'\n", cmd);
    return 1;
}
