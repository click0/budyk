// SPDX-License-Identifier: BSD-3-Clause
// budyk — lightweight FreeBSD server monitoring with adaptive collection
//
// Usage:
//   budyk serve [--config config.yaml]
//   budyk tui
//   budyk hash-password
//   budyk suggest-rules [--config PATH] [--window 7d] [--output rules.lua] [--ai --api-key KEY]
//   budyk version

#include "ai/suggest.h"
#include "config/config.h"
#include "core/codec.h"
#include "core/sample.h"
#include "storage/codec.h"
#include "storage/ring_file.h"
#include "web/auth.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

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

// Parse "<num><unit>" duration strings (s / m / h / d) → ns. Returns 0
// on bad input. Caps at one year so a 0-padded uint64 multiplication
// can't overflow.
uint64_t parse_window_ns(const char* s) {
    if (s == nullptr || *s == '\0') return 0;
    char* end = nullptr;
    long n = std::strtol(s, &end, 10);
    if (n <= 0 || end == s || *end == '\0' || *(end + 1) != '\0') return 0;
    uint64_t mult = 0;
    switch (*end) {
        case 's': mult = 1000000000ULL;          break;
        case 'm': mult = 60ULL * 1000000000ULL;  break;
        case 'h': mult = 3600ULL * 1000000000ULL; break;
        case 'd': mult = 86400ULL * 1000000000ULL; break;
        default: return 0;
    }
    constexpr uint64_t kMaxSec = 366ULL * 86400ULL;
    if (static_cast<uint64_t>(n) * (mult / 1000000000ULL) > kMaxSec) return 0;
    return static_cast<uint64_t>(n) * mult;
}

uint64_t now_realtime_ns() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Read up to `cap` most-recent samples from `ring_path` whose
// timestamp_nanos is within [now - window_ns, now]. Records that fail
// to decode (CRC mismatch / version skew) are silently skipped.
int load_samples_for_suggest(const char* ring_path,
                             uint32_t record_size,
                             uint64_t cap,
                             uint64_t window_ns,
                             std::vector<budyk::Sample>* out) {
    budyk::RingFile ring;
    if (ring.open(ring_path, /*tier*/1, record_size, cap) != 0) return -1;

    const uint64_t widx     = ring.write_index();
    const uint64_t valid    = widx < cap ? widx : cap;
    const uint64_t cutoff   = now_realtime_ns() - window_ns;

    std::vector<uint8_t> buf(record_size);
    out->reserve(static_cast<size_t>(valid));

    // Walk every valid slot. The ring file's slots are ring-rotated when
    // widx > cap; record_decode tolerates that by reading the framing
    // independent of slot order.
    for (uint64_t i = 0; i < valid; ++i) {
        if (ring.read_at(i, buf.data(), record_size) != 0) continue;
        budyk::Sample s{};
        if (budyk::record_decode(buf.data(), record_size, &s) != 0) continue;
        if (window_ns > 0 && s.timestamp_nanos < cutoff)              continue;
        out->push_back(s);
    }
    ring.close();
    return 0;
}

int cmd_suggest_rules(int argc, char* argv[]) {
    const char* config_path = "/usr/local/etc/budyk/config.yaml";
    const char* window_arg  = "7d";
    const char* output_path = nullptr;       // nullptr → stdout
    bool ai = false;

    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            window_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--ai") == 0) {
            ai = true;
        } else if (std::strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
            ++i;                              // accept but ignore for now
        } else {
            std::fprintf(stderr, "budyk suggest-rules: unknown arg '%s'\n", argv[i]);
            return 1;
        }
    }

    if (ai) {
        std::fprintf(stderr,
            "budyk suggest-rules: --ai (Tier B / LLM) is not implemented yet; "
            "use Tier A (no --ai) for now.\n");
        return 1;
    }

    const uint64_t window_ns = parse_window_ns(window_arg);
    if (window_ns == 0) {
        std::fprintf(stderr,
            "budyk suggest-rules: invalid --window '%s' "
            "(expected <N>{s,m,h,d}, max ~1y)\n", window_arg);
        return 1;
    }

    budyk::Config cfg;
    if (budyk::config_load(config_path, &cfg) != 0) {
        std::fprintf(stderr,
            "budyk suggest-rules: failed to load config '%s'\n", config_path);
        return 1;
    }

    char ring_path[1024];
    if (std::snprintf(ring_path, sizeof(ring_path),
                      "%s/tier1.ring", cfg.data_dir) >= static_cast<int>(sizeof(ring_path))) {
        std::fprintf(stderr, "budyk suggest-rules: data_dir path too long\n");
        return 1;
    }

    const uint32_t record_size = static_cast<uint32_t>(budyk::record_size_for_sample());
    uint64_t cap = (static_cast<uint64_t>(cfg.tier1_max_mb) * 1024ULL * 1024ULL) / record_size;
    if (cap == 0) cap = 1;

    std::vector<budyk::Sample> samples;
    if (load_samples_for_suggest(ring_path, record_size, cap, window_ns, &samples) != 0) {
        std::fprintf(stderr,
            "budyk suggest-rules: failed to open '%s' "
            "(run `budyk serve` first to collect data)\n", ring_path);
        return 1;
    }

    const std::string doc = budyk::suggest_rules_for_samples(
        samples.data(), samples.size());

    if (output_path != nullptr) {
        std::FILE* f = std::fopen(output_path, "w");
        if (f == nullptr) {
            std::fprintf(stderr,
                "budyk suggest-rules: open '%s': %s\n",
                output_path, std::strerror(errno));
            return 1;
        }
        std::fwrite(doc.data(), 1, doc.size(), f);
        std::fclose(f);
        std::fprintf(stderr,
            "budyk suggest-rules: wrote %zu bytes to %s "
            "(%zu samples in window %s)\n",
            doc.size(), output_path, samples.size(), window_arg);
    } else {
        std::fputs(doc.c_str(), stdout);
    }
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
        return cmd_suggest_rules(argc, argv);
    }

    std::fprintf(stderr, "budyk: unknown command '%s'\n", cmd);
    return 1;
}
