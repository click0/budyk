// SPDX-License-Identifier: BSD-3-Clause
#include "cli/commands.h"

#include "ai/baseline.h"
#include "ai/llm_client.h"
#include "ai/suggest.h"
#include "config/config.h"
#include "core/clock.h"
#include "core/parse_int.h"
#include "core/sample.h"
#include "security/file_watcher.h"
#include "storage/codec.h"
#include "storage/ring_file.h"
#include "tui/tui.h"
#include "web/auth.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
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
// on bad input or more than a year. The number is read whole and in
// range (parse_int_full): strtol() and a multiplication that wrapped took
// 213503982334602d for about 17 hours.
uint64_t parse_window_ns(const char* s) {
    if (s == nullptr || *s == '\0') return 0;
    const size_t len = std::strlen(s);
    uint64_t unit_s = 0;
    switch (s[len - 1]) {
        case 's': unit_s = 1;     break;
        case 'm': unit_s = 60;    break;
        case 'h': unit_s = 3600;  break;
        case 'd': unit_s = 86400; break;
        default: return 0;
    }
    constexpr uint64_t kMaxSec = 366ULL * 86400ULL;
    const std::string digits(s, len - 1);
    long n = 0;
    if (!parse_int_full(digits.c_str(), 1, static_cast<long>(kMaxSec / unit_s), &n)) return 0;
    return static_cast<uint64_t>(n) * unit_s * 1000000000ULL;
}

// Read up to `cap` most-recent samples from `ring_path` whose
// timestamp_nanos is within [now - window_ns, now]. Records that fail
// to decode (CRC mismatch / version skew) are silently skipped.
int load_samples_for_suggest(const char* ring_path,
                             uint8_t tier,
                             uint32_t record_size,
                             uint64_t cap,
                             uint64_t window_ns,
                             std::vector<budyk::Sample>* out) {
    // Read only: the daemon owns the rings (and running this as root must
    // not leave a root-owned ring behind for it).
    budyk::RingFile ring;
    if (ring.open_read_only(ring_path, tier, record_size, cap) != 0) return -1;

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
    out += "cpu_count=" + std::to_string(budyk::newest_cpu_count(s, n)) + "\n";
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

namespace {

// The terminal as it was before read_password() turned echo off, for a
// signal that ends the program while it waits: Ctrl-C at the prompt left
// the shell typing blind (dash, sh; bash puts it back itself).
struct termios g_tty_saved;
volatile sig_atomic_t g_tty_echo_off = 0;
const int kTtySignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};

// TCSANOW: a handler must not wait for the output to drain (TCSAFLUSH
// does, and a terminal nobody reads, or one stopped by ^S, never drains).
void restore_tty_and_reraise(int sig) {
    if (g_tty_echo_off) ::tcsetattr(STDIN_FILENO, TCSANOW, &g_tty_saved);
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

} // namespace

// Read a line from stdin without echoing it. Falls back to a plain
// getline if stdin is not a tty (piped input — caller already knows
// the password is fine to be visible).
std::string read_password(const char* prompt) {
    std::fputs(prompt, stderr);
    std::fflush(stderr);

    const bool is_tty = ::isatty(STDIN_FILENO);
    struct sigaction old_actions[sizeof(kTtySignals) / sizeof(kTtySignals[0])];
    if (is_tty && ::tcgetattr(STDIN_FILENO, &g_tty_saved) == 0) {
        struct sigaction sa{};
        sa.sa_handler = restore_tty_and_reraise;
        ::sigemptyset(&sa.sa_mask);
        // A signal the caller ignores (nohup, a non-interactive shell's
        // background job) stays ignored, as readpassphrase(3) leaves it.
        for (size_t i = 0; i < sizeof(kTtySignals) / sizeof(kTtySignals[0]); ++i) {
            ::sigaction(kTtySignals[i], nullptr, &old_actions[i]);
            if (old_actions[i].sa_handler != SIG_IGN) ::sigaction(kTtySignals[i], &sa, nullptr);
        }
        struct termios noecho = g_tty_saved;
        noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        g_tty_echo_off = 1;
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
    }

    std::string line;
    std::getline(std::cin, line);

    if (g_tty_echo_off) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_tty_saved);
        g_tty_echo_off = 0;
        for (size_t i = 0; i < sizeof(kTtySignals) / sizeof(kTtySignals[0]); ++i) {
            ::sigaction(kTtySignals[i], &old_actions[i], nullptr);
        }
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
    const char* config_path = budyk::default_config_path();
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

    // The L2 and L1 rings (spec §3.7.1: Tier 2 / Tier 3): the L1 ring has
    // a record each L1 interval whatever ran (raw, or folded from a finer
    // level), the L2 ring one each L2 interval while L2 or finer ran. The
    // L3 ring alone, read before, holds only what was collected while a
    // dashboard was open: in adaptive mode, often nothing at all.
    const uint32_t record_size = static_cast<uint32_t>(budyk::record_size_for_sample());
    constexpr uint64_t kMiB = 1024 * 1024;
    auto load = [&](const char* file, uint8_t tier, int max_mb, std::vector<budyk::Sample>* out) {
        const std::string path = std::string(cfg.data_dir) + "/" + file;
        uint64_t cap = (static_cast<uint64_t>(max_mb) * kMiB) / record_size;
        if (cap == 0) cap = 1;
        return load_samples_for_suggest(path.c_str(), tier, record_size, cap, window_ns, out);
    };
    std::vector<budyk::Sample> l2, l1;
    const int rc2 = load("tier2.ring", 2, cfg.tier2_max_mb, &l2);
    const int rc1 = load("tier3.ring", 3, cfg.tier3_max_mb, &l1);
    if (rc2 != 0 && rc1 != 0) {
        std::fprintf(stderr,
            "budyk suggest-rules: cannot read '%s/tier2.ring' or '%s/tier3.ring' "
            "(run `budyk serve` first to collect data)\n", cfg.data_dir, cfg.data_dir);
        return 1;
    }
    const auto by_time = [](const budyk::Sample& a, const budyk::Sample& b) {
        return a.timestamp_nanos < b.timestamp_nanos;
    };
    std::sort(l2.begin(), l2.end(), by_time);
    std::sort(l1.begin(), l1.end(), by_time);
    // Each stretch once, at the finer level: an L1 record whose interval
    // has L2 records is left out.
    const uint64_t l1_ns = static_cast<uint64_t>(cfg.scheduler.l1_interval_sec) * uint64_t{1000000000};
    std::vector<budyk::Sample> samples = l2;
    for (const auto& r : l1) {
        const uint64_t from = r.timestamp_nanos > l1_ns ? r.timestamp_nanos - l1_ns : 0;
        const auto it = std::lower_bound(l2.begin(), l2.end(), from,
            [](const budyk::Sample& s, uint64_t t) { return s.timestamp_nanos < t; });
        if (it == l2.end() || it->timestamp_nanos > r.timestamp_nanos) samples.push_back(r);
    }
    std::sort(samples.begin(), samples.end(), by_time);

    std::string doc;
    if (ai) {
        const std::string summary = build_llm_summary(samples.data(), samples.size());
        std::string why;
        if (budyk::suggest_rules_llm(api_key, summary, &doc, &why) != 0) {
            std::fprintf(stderr, "budyk suggest-rules: LLM call failed: %s\n", why.c_str());
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
        std::FILE* f = std::fopen(output_path, "we");
        if (f == nullptr) {
            std::fprintf(stderr,
                "budyk suggest-rules: open '%s': %s\n",
                output_path, std::strerror(errno));
            return 1;
        }
        // A write that fails (a full disk) is said so: it reported the
        // bytes written and exit 0.
        const bool wrote = std::fwrite(doc.data(), 1, doc.size(), f) == doc.size();
        const int  werr  = wrote ? 0 : errno;
        if (std::fclose(f) != 0 || !wrote) {
            std::fprintf(stderr, "budyk suggest-rules: writing '%s': %s\n",
                         output_path, std::strerror(werr != 0 ? werr : errno));
            return 1;
        }
        std::fprintf(stderr,
            "budyk suggest-rules: wrote %zu bytes to %s "
            "(%zu samples in window %s)\n",
            doc.size(), output_path, samples.size(), window_arg);
    } else if (std::fputs(doc.c_str(), stdout) == EOF || std::fflush(stdout) != 0) {
        std::fprintf(stderr, "budyk suggest-rules: writing the standard output: %s\n",
                     std::strerror(errno));
        return 1;
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
            long ms = 0;
            if (!parse_int_full(argv[++i], -1, INT_MAX, &ms)) {
                std::fprintf(stderr,
                    "budyk watch-files: --timeout wants milliseconds "
                    "(-1 for none), got '%s'\n", argv[i]);
                return 1;
            }
            timeout_ms = static_cast<int>(ms);
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
    if (const int rc = fw.init(); rc != 0) {
        std::fprintf(stderr,
            "budyk watch-files: the file watcher cannot start: %s\n", std::strerror(-rc));
        return 1;
    }
    for (const auto& p : paths) {
        const int rc = fw.add(p);
        if (rc < 0) {
            std::fprintf(stderr,
                "budyk watch-files: cannot watch '%s': %s\n",
                p.c_str(), std::strerror(-rc));
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
                long p = 0;
                if (!parse_int_full(argv[++i], 1, 65535, &p)) {
                    std::fprintf(stderr,
                        "budyk tui: --port wants a number from 1 to 65535, got '%s'\n",
                        argv[i]);
                    return 1;
                }
                port = static_cast<int>(p);
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
