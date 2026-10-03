// SPDX-License-Identifier: BSD-3-Clause
#include "cli/commands.h"

#include "ai/baseline.h"
#include "ai/llm_client.h"
#include "ai/suggest.h"
#include "config/config.h"
#include "core/clock.h"
#include "core/sample.h"
#include "security/file_watcher.h"
#include "storage/codec.h"
#include "storage/ring_file.h"
#include "tui/tui.h"
#include "web/auth.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <termios.h>
#include <unistd.h>

namespace budyk {

namespace {

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

// Format a MetricBaseline as a one-line digest for the LLM prompt.
std::string fmt_baseline_line(const char* name, const budyk::MetricBaseline& b) {
    if (b.n == 0) return {};
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "%s n=%zu min=%.2f max=%.2f mean=%.2f stddev=%.2f p95=%.2f p99=%.2f\n",
        name, b.n, b.min, b.max, b.mean, b.stddev, b.p95, b.p99);
    return buf;
}

std::string build_llm_summary(const budyk::Sample* s, size_t n) {
    if (s == nullptr || n == 0) return "no samples available\n";
    std::string out;
    out += "samples=" + std::to_string(n) + "\n";
    out += "cpu_count=" + std::to_string(s[n - 1].cpu.count) + "\n";
    out += fmt_baseline_line("cpu.total_percent",
                             budyk::compute_cpu_total_percent_stats(s, n));
    out += fmt_baseline_line("mem.available_percent",
                             budyk::compute_mem_available_percent_stats(s, n));
    out += fmt_baseline_line("swap.used_percent",
                             budyk::compute_swap_used_percent_stats(s, n));
    out += fmt_baseline_line("load.avg_1m",
                             budyk::compute_load_1m_stats(s, n));
    out += fmt_baseline_line("disk.read_bytes_per_sec",
                             budyk::compute_disk_read_bytes_per_sec_stats(s, n));
    out += fmt_baseline_line("disk.write_bytes_per_sec",
                             budyk::compute_disk_write_bytes_per_sec_stats(s, n));
    out += fmt_baseline_line("net.rx_bytes_per_sec",
                             budyk::compute_net_rx_bytes_per_sec_stats(s, n));
    out += fmt_baseline_line("net.tx_bytes_per_sec",
                             budyk::compute_net_tx_bytes_per_sec_stats(s, n));
    return out;
}

} // namespace

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

int cmd_suggest_rules(int argc, char* argv[]) {
    const char* config_path = "/usr/local/etc/budyk/config.yaml";
    const char* window_arg  = "7d";
    const char* output_path = nullptr;       // nullptr → stdout
    const char* api_key     = nullptr;
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
            api_key = argv[++i];
        } else {
            std::fprintf(stderr, "budyk suggest-rules: unknown arg '%s'\n", argv[i]);
            return 1;
        }
    }

    if (ai && (api_key == nullptr || *api_key == '\0')) {
        const char* env = std::getenv("ANTHROPIC_API_KEY");
        if (env != nullptr && *env != '\0') {
            api_key = env;
        } else {
            std::fprintf(stderr,
                "budyk suggest-rules: --ai requires --api-key or "
                "ANTHROPIC_API_KEY env var\n");
            return 1;
        }
    }

    const uint64_t window_ns = parse_window_ns(window_arg);
    if (window_ns == 0) {
        std::fprintf(stderr,
            "budyk suggest-rules: invalid --window '%s' "
            "(expected <N>{s,m,h,d}, max ~1y)\n", window_arg);
        return 1;
    }

    budyk::Config cfg;
    std::string cfg_err;
    if (budyk::config_load(config_path, &cfg, &cfg_err) != 0) {
        std::fprintf(stderr,
            "budyk suggest-rules: failed to load config '%s': %s\n", config_path,
            cfg_err.c_str());
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

    std::string doc;
    if (ai) {
        const std::string summary = build_llm_summary(samples.data(), samples.size());
        const int rc = budyk::suggest_rules_llm(api_key, summary, &doc);
        if (rc != 0) {
            std::fprintf(stderr,
                "budyk suggest-rules: LLM call failed (rc=%d). "
                "Check the API key, network, and that curl(1) is on PATH.\n", rc);
            return 1;
        }
        // Prefix the doc with a header so the user knows it's Tier B.
        doc = "-- AI-suggested rules (Tier B, LLM-generated). Review before using.\n"
              "-- Generated from " + std::to_string(samples.size()) +
              " samples in window " + window_arg + ".\n\n" + doc;
    } else {
        doc = budyk::suggest_rules_for_samples(samples.data(), samples.size());
    }

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

// Watch one or more file paths for content / metadata changes and
// print events as they arrive. Useful for one-shot diagnostic, for
// example "is something silently touching /etc/sudoers?".
//
//   budyk watch-files [--timeout MS] <path> [<path>...]
//
// Default timeout is 30s; pass --timeout -1 to block indefinitely.
// Exits 0 on EOF (timeout reached) or when SIGTERM/SIGINT arrives.
int cmd_watch_files(int argc, char* argv[]) {
    int                       timeout_ms = 30000;
    std::vector<std::string>  paths;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = std::atoi(argv[++i]);
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr,
                "budyk watch-files: unknown arg '%s'\n", argv[i]);
            return 1;
        } else {
            paths.emplace_back(argv[i]);
        }
    }
    if (paths.empty()) {
        std::fprintf(stderr,
            "usage: budyk watch-files [--timeout MS] <path> [<path>...]\n");
        return 1;
    }

    budyk::FileWatcher fw;
    if (fw.init() != 0) {
        std::fprintf(stderr,
            "budyk watch-files: FileWatcher.init() failed\n");
        return 1;
    }
    for (const auto& p : paths) {
        const int rc = fw.add(p);
        if (rc < 0) {
            std::fprintf(stderr,
                "budyk watch-files: cannot watch '%s' (errno=%d)\n",
                p.c_str(), -rc);
        }
    }
    std::printf("watching %zu file(s); timeout=%dms\n",
                static_cast<std::size_t>(fw.count()), timeout_ms);

    std::vector<budyk::FileChangeEvent> events;
    const int n = fw.poll(timeout_ms, &events);
    if (n < 0) {
        std::fprintf(stderr,
            "budyk watch-files: poll failed: %s\n", std::strerror(-n));
        return 1;
    }
    if (n == 0) {
        std::printf("(no events within timeout)\n");
        return 0;
    }
    for (const auto& ev : events) {
        const char* kind =
            ev.kind == budyk::FileChangeKind::Deleted  ? "deleted"  :
            ev.kind == budyk::FileChangeKind::Created  ? "created"  :
                                                         "modified";
        std::printf("%-8s  %s\n", kind, ev.path.c_str());
    }
    return 0;
}

int cmd_tui(int argc, char* argv[]) {
        const char* host = nullptr;
        int         port = 0;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
                host = argv[++i];
            } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                port = std::atoi(argv[++i]);
            } else {
                std::fprintf(stderr, "budyk tui: unknown arg '%s'\n", argv[i]);
                return 1;
            }
        }
        // Asked only if the daemon answers 401 (web.auth.enabled).
        return tui_run(host, port, [] {
            return read_password("budyk password: ");
        }) == 0 ? 0 : 1;
}

} // namespace budyk
