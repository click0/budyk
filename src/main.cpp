// SPDX-License-Identifier: BSD-3-Clause
// budyk — lightweight FreeBSD server monitoring with adaptive collection
//
// Usage:
//   budyk serve [--config config.yaml]
//   budyk tui
//   budyk hash-password
//   budyk suggest-rules [--config PATH] [--window 7d] [--output rules.lua] [--ai --api-key KEY]
//   budyk version

#include "ai/baseline.h"
#include "ai/llm_client.h"
#include "ai/suggest.h"
#include "config/config.h"
#include "core/codec.h"
#include "core/sample.h"
#include "core/sample_c.h"
#include "hot_buffer/hot_buffer.h"
#include "rules/alert.h"
#include "rules/lua_engine.h"
#include "rules/yaml_compat.h"
#include "scheduler/scheduler.h"
#include "security/file_watcher.h"
#include "storage/codec.h"
#include "storage/ring_file.h"
#include "storage/tier_manager.h"
#include "tui/tui.h"
#include "web/auth.h"
#include "web/json.h"
#include "web/server.h"
#include "web/session.h"
#include "web/spa.h"
#include "web/ws_hub.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <sys/socket.h>
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

// ----------------------------------------------------------------------------
// `budyk serve` — main daemon loop.
// ----------------------------------------------------------------------------
// The collector runs in the foreground thread (fits the spec MVP — a real
// thread-pool wakes up later when the WS hub joins the picture). Each tick:
//   1. resolve the cadence from the scheduler's current Level,
//   2. nanosleep until the next deadline (interruptible by SIGINT/SIGTERM),
//   3. populate a Sample from the platform collectors,
//   4. push it through Scheduler.tick() to update the level,
//   5. store via TierManager + push to HotBuffer + eval_tick on LuaEngine.
//
// Platform collectors are gated on BUDYK_PLATFORM via the budyk_collector
// static lib. We use the C-shim Sample type for collector calls and copy
// the relevant fields back into budyk::Sample for the rest of the pipeline.

static volatile std::sig_atomic_t g_stop   = 0;
static volatile std::sig_atomic_t g_reload = 0;

extern "C" void serve_signal_handler(int sig) {
    if (sig == SIGHUP) g_reload = 1;
    else               g_stop   = 1;
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = serve_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                      // no SA_RESTART — let nanosleep return
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP,  &sa, nullptr);   // → rules reload

    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &ign, nullptr);
}

int level_interval_sec(budyk::Level lv, const budyk::SchedulerConfig& sc) {
    switch (lv) {
        case budyk::Level::L1: return sc.l1_interval_sec;
        case budyk::Level::L2: return sc.l2_interval_sec;
        case budyk::Level::L3: return sc.l3_interval_sec;
    }
    return sc.l3_interval_sec;
}

// Collect one tick into `s`. Stateful collectors (CPU / disk / net) keep
// their delta context across ticks via the budyk_*_ctx_c args.
void collect_one(budyk::Sample* s,
                 budyk_cpu_ctx_c*  cpu_ctx,
                 budyk_disk_ctx_c* disk_ctx,
                 budyk_net_ctx_c*  net_ctx) {
    budyk_sample_c c{};
    c.timestamp_nanos = s->timestamp_nanos;

#if defined(BUDYK_LINUX)
    budyk_collect_cpu_linux    (cpu_ctx, &c);
    budyk_collect_memory_linux (&c);
    budyk_collect_uptime_linux (&c);
    budyk_collect_load_linux   (&c);
    budyk_collect_disk_linux   (disk_ctx, &c);
    budyk_collect_network_linux(net_ctx,  &c);
    budyk_collect_proc_linux   (&c);
    budyk_collect_entropy_linux(&c);
    budyk_collect_self_linux   (&c);
    budyk_collect_thermal_linux(&c);
#elif defined(BUDYK_FREEBSD)
    budyk_collect_cpu_freebsd    (cpu_ctx, &c);
    budyk_collect_memory_freebsd (&c);
    budyk_collect_uptime_freebsd (&c);
    budyk_collect_load_freebsd   (&c);
    budyk_collect_disk_freebsd   (disk_ctx, &c);
    budyk_collect_network_freebsd(net_ctx,  &c);
    budyk_collect_proc_freebsd   (&c);
    budyk_collect_entropy_freebsd(&c);
    budyk_collect_self_freebsd   (&c);
    budyk_collect_thermal_freebsd(&c);
#else
    (void)cpu_ctx; (void)disk_ctx; (void)net_ctx;
#endif

    // Copy the C-shim back into the C++ Sample (same field names, scalar
    // types match by construction in core/sample_c.h).
    s->cpu.total_percent      = c.cpu.total_percent;
    s->cpu.count              = c.cpu.count;
    s->mem.total              = c.mem.total;
    s->mem.available          = c.mem.available;
    s->mem.available_percent  = c.mem.available_percent;
    s->swap.total             = c.swap.total;
    s->swap.used              = c.swap.used;
    s->swap.used_percent      = c.swap.used_percent;
    s->load.avg_1m            = c.load.avg_1m;
    s->load.avg_5m            = c.load.avg_5m;
    s->load.avg_15m           = c.load.avg_15m;
    s->disk.read_bytes_per_sec  = c.disk.read_bytes_per_sec;
    s->disk.write_bytes_per_sec = c.disk.write_bytes_per_sec;
    s->disk.device_count        = c.disk.device_count;
    s->net.rx_bytes_per_sec     = c.net.rx_bytes_per_sec;
    s->net.tx_bytes_per_sec     = c.net.tx_bytes_per_sec;
    s->net.interface_count      = c.net.interface_count;
    s->proc.total              = c.proc.total;
    s->proc.running            = c.proc.running;
    s->entropy.available_bits  = c.entropy.available_bits;
    s->entropy.present         = (c.entropy.present != 0);
    s->self_.rss_bytes          = c.self_.rss_bytes;
    s->self_.peak_rss_bytes     = c.self_.peak_rss_bytes;
    s->self_.cpu_user_seconds   = c.self_.cpu_user_seconds;
    s->self_.cpu_system_seconds = c.self_.cpu_system_seconds;
    s->thermal.max_celsius      = c.thermal.max_celsius;
    s->thermal.sensor_count     = c.thermal.sensor_count;
    s->thermal.present          = (c.thermal.present != 0);
    s->uptime_seconds         = c.uptime_seconds;
}

// Pull the value of a top-level string field out of a tiny JSON object.
// Looks for `"<key>"<ws>:<ws>"<value>"`. Doesn't handle escapes — the
// daemon's only JSON input today is a password from the login form,
// which is rejected at length cap; anything fancy fails closed.
bool json_get_string(const std::string& body, const char* key, std::string* out) {
    std::string needle = "\"";
    needle.append(key);
    needle.append("\"");
    auto kpos = body.find(needle);
    if (kpos == std::string::npos) return false;
    auto colon = body.find(':', kpos + needle.size());
    if (colon == std::string::npos) return false;
    auto open = body.find('"', colon + 1);
    if (open == std::string::npos) return false;
    auto close = body.find('"', open + 1);
    if (close == std::string::npos) return false;
    out->assign(body, open + 1, close - open - 1);
    return true;
}

// Extract the value of a single cookie name from a Cookie header line
// like "a=1; b=2". Returns empty string if missing.
std::string cookie_value(const std::string& cookie_header, const char* name) {
    const std::string needle = std::string(name) + "=";
    size_t p = 0;
    while (p < cookie_header.size()) {
        size_t end = cookie_header.find(';', p);
        if (end == std::string::npos) end = cookie_header.size();
        size_t start = p;
        while (start < end && (cookie_header[start] == ' ' || cookie_header[start] == '\t'))
            ++start;
        if (cookie_header.compare(start, needle.size(), needle) == 0) {
            return cookie_header.substr(start + needle.size(), end - start - needle.size());
        }
        p = end + 1;
    }
    return {};
}

// Split a request target into path + raw query string. "/api/range?x=1"
// → path "/api/range", query "x=1". No query → query stays empty.
void split_target(const std::string& target,
                  std::string* path, std::string* query) {
    const size_t q = target.find('?');
    if (q == std::string::npos) {
        *path  = target;
        query->clear();
    } else {
        *path  = target.substr(0, q);
        *query = target.substr(q + 1);
    }
}

// Pull a single unsigned 64-bit value for `key` out of a urlencoded
// query string ("a=1&b=2"). Returns `fallback` when the key is absent
// or doesn't parse as a non-negative integer. Only digits are accepted
// — no signs, no units (callers pass nanoseconds / counts directly).
uint64_t query_u64(const std::string& query, const char* key, uint64_t fallback) {
    const std::string needle = std::string(key) + "=";
    size_t p = 0;
    while (p < query.size()) {
        size_t amp = query.find('&', p);
        if (amp == std::string::npos) amp = query.size();
        if (query.compare(p, needle.size(), needle) == 0) {
            const std::string val = query.substr(p + needle.size(),
                                                 amp - p - needle.size());
            if (val.empty()) return fallback;
            uint64_t out = 0;
            for (char c : val) {
                if (c < '0' || c > '9') return fallback;
                out = out * 10 + static_cast<uint64_t>(c - '0');
            }
            return out;
        }
        p = amp + 1;
    }
    return fallback;
}

// Sleep for at most `seconds` real-time, returning early if a signal sets
// g_stop. Safe to call with seconds <= 0 (no-op).
void interruptible_sleep(int seconds) {
    if (seconds <= 0 || g_stop) return;
    struct timespec req{seconds, 0}, rem{};
    while (::nanosleep(&req, &rem) != 0) {
        if (errno != EINTR || g_stop) return;
        req = rem;
    }
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
            "budyk watch-files: poll failed (errno=%d)\n", -n);
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

int cmd_serve(int argc, char* argv[]) {
    const char* config_path     = "/usr/local/etc/budyk/config.yaml";
    bool        cli_enable_exec   = false;
    bool        cli_enable_freeze = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--enable-exec") == 0) {
            cli_enable_exec = true;
        } else if (std::strcmp(argv[i], "--enable-freeze") == 0) {
            cli_enable_freeze = true;
        } else {
            std::fprintf(stderr, "budyk serve: unknown arg '%s'\n", argv[i]);
            return 1;
        }
    }

    budyk::Config cfg;
    if (budyk::config_load(config_path, &cfg) != 0) {
        std::fprintf(stderr,
            "budyk serve: failed to load config '%s'\n", config_path);
        return 1;
    }
    // CLI flags override config (operator intent on the command line wins).
    if (cli_enable_exec)   cfg.rules_enable_exec   = true;
    if (cli_enable_freeze) cfg.rules_enable_freeze = true;

    install_signal_handlers();

    budyk::TierManager tm;
    if (tm.init(cfg.data_dir,
                cfg.tier1_max_mb, cfg.tier2_max_mb, cfg.tier3_max_mb) != 0) {
        std::fprintf(stderr,
            "budyk serve: TierManager.init('%s') failed\n", cfg.data_dir);
        return 1;
    }

    budyk::HotBuffer hot(static_cast<size_t>(cfg.hot_buffer_capacity));
    // hot is read by the HTTP thread (/api/samples) and written by this
    // collector thread; HotBuffer itself isn't synchronised, so wrap
    // both sides in a mutex. Single-admin traffic + one push per tick
    // means contention is essentially zero.
    std::mutex hot_mtx;

    budyk::Scheduler sched(cfg.scheduler);

    budyk::LuaEngine engine;

    // Both initial setup and SIGHUP reload do the same dance: init the
    // engine, apply exec/freeze gates + allowlists, load rules (yaml or
    // lua by extension), register alert channels, then optionally
    // restore per-rule state from `restore_path` so cooldowns survive.
    // Factored into a lambda so the reload path can't drift away from
    // initial setup. Returns the engine.init rc (0 on success).
    auto setup_engine = [&](const char* restore_path) -> int {
        if (engine.init(cfg.rules_enable_exec) != 0) return -1;
        if (!cfg.rules_exec_allow.empty()) {
            engine.set_exec_allowlist(cfg.rules_exec_allow);
        }
        engine.set_freeze_enabled(cfg.rules_enable_freeze);
        if (!cfg.rules_freeze_allow.empty()) {
            engine.set_freeze_allowlist(cfg.rules_freeze_allow);
        }
        if (cfg.rules_path[0] != '\0' && ::access(cfg.rules_path, R_OK) == 0) {
            // Dispatch by extension: .yaml / .yml goes through the
            // simple-YAML transpiler first, anything else is fed to Lua
            // verbatim. The transpiler emits regular watch() calls, so
            // the engine sees no difference downstream.
            const size_t plen = std::strlen(cfg.rules_path);
            const bool   is_yaml =
                (plen >= 5 && std::strcmp(cfg.rules_path + plen - 5, ".yaml") == 0) ||
                (plen >= 4 && std::strcmp(cfg.rules_path + plen - 4, ".yml")  == 0);
            int rc;
            if (is_yaml) {
                std::string lua_src;
                rc = budyk::yaml_rules_to_lua_file(cfg.rules_path, &lua_src);
                if (rc == 0) rc = engine.load_string(lua_src.c_str());
            } else {
                rc = engine.load_file(cfg.rules_path);
            }
            if (rc != 0) {
                std::fprintf(stderr,
                    "budyk serve: rules file '%s' failed to load (rc=%d) — continuing without rules\n",
                    cfg.rules_path, rc);
            }
        }
        // Restore per-rule cooldown / fire counters. Must come after
        // load_file/load_string (matches saved entries to rules by name).
        if (restore_path != nullptr) {
            engine.load_state(restore_path);
        }
        // Register configured alert channels so rules can call
        // alert(name, severity, message) and reach them. Re-applied on
        // reload because shutdown() clears the engine's dispatcher.
        for (const auto& src : cfg.alert_channels) {
            budyk::AlertChannel ch;
            ch.name  = src.name;
            ch.type  = src.type;
            ch.url   = src.url;
            ch.topic = src.topic;
            ch.token = src.token;
            ch.from  = src.from;
            engine.alerts().add_channel(std::move(ch));
        }
        return 0;
    };

    // Initial: derive state_path now; restore on init.
    std::string state_path;
    if (cfg.rules_persist_state) {
        state_path = cfg.rules_state_path[0] != '\0'
                   ? std::string(cfg.rules_state_path)
                   : std::string(cfg.data_dir) + "/rule_state.tsv";
    }
    if (setup_engine(state_path.empty() ? nullptr : state_path.c_str()) != 0) {
        std::fprintf(stderr, "budyk serve: LuaEngine.init failed\n");
        tm.close();
        return 1;
    }
    if (!cfg.alert_channels.empty()) {
        std::fprintf(stderr,
            "budyk serve: registered %zu alert channel(s)\n",
            cfg.alert_channels.size());
    }

    // SIGHUP reload: persist current cooldowns to a transient file,
    // shutdown the engine, set it up again, restore from that file.
    // Cooldowns / fire_count survive even when rules.persist_state is
    // disabled — the transient file lives only across the swap.
    auto reload_rules = [&]() {
        const std::string rp = std::string(cfg.data_dir) + "/.reload.tsv";
        engine.save_state(rp.c_str());
        engine.shutdown();
        if (setup_engine(rp.c_str()) != 0) {
            std::fprintf(stderr,
                "budyk serve: SIGHUP — engine re-init failed; daemon is now ruleless\n");
        } else {
            std::fprintf(stderr,
                "budyk serve: SIGHUP — rules reloaded (%d rule(s))\n",
                engine.rule_count());
        }
        ::unlink(rp.c_str());
    };

    budyk::HttpServer    http;
    budyk::SessionStore  sessions;       // 24-h default TTL
    budyk::WebSocketHub  ws;

    // Restore logged-in sessions across restarts. The file holds live
    // bearer tokens (mode 0600); load drops any already past their TTL.
    if (cfg.auth_persist_sessions) {
        const std::string session_path =
            std::string(cfg.data_dir) + "/sessions.tsv";
        sessions.load(session_path.c_str());
        sessions.set_persist_path(session_path);   // autosave on mutation
    }

    auto authed = [&cfg, &sessions](const budyk::HttpRequest& req) -> bool {
        if (!cfg.auth_enabled) return true;
        const std::string c = req.header("Cookie");
        if (c.empty())        return false;
        const std::string tok = cookie_value(c, "budyk_session");
        return !tok.empty() && sessions.verify(tok);
    };

    auto router = [&cfg, &hot, &hot_mtx, &tm, &sessions, &ws, &authed](const budyk::HttpRequest& req) {
        // Static SPA — served at /, /index.html and /budyk for the
        // browser-friendly entry. Always public; the JS itself does
        // the auth-probe + login round-trip.
        if (req.method == "GET" &&
            (req.path == "/" || req.path == "/index.html")) {
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "text/html; charset=utf-8";
            r.body.assign(budyk::kSpaIndexHtml, budyk::kSpaIndexHtmlLen);
            return r;
        }

        // Health is always public so liveness probes work pre-auth.
        if (req.method == "GET" && req.path == "/api/health") {
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "application/json";
            r.body =
                "{\"status\":\"ok\","
                "\"version\":\"0.4.0\","
                "\"data_dir\":\"" + std::string(cfg.data_dir) + "\"}\n";
            return r;
        }

        if (req.method == "POST" && req.path == "/api/auth/login") {
            if (!cfg.auth_enabled || cfg.password_hash[0] == '\0') {
                return budyk::HttpResponse{
                    403, "text/plain", "auth disabled\n", {}};
            }
            std::string pw;
            if (!json_get_string(req.body, "password", &pw) || pw.empty()) {
                return budyk::HttpResponse{
                    400, "application/json",
                    "{\"error\":\"missing password\"}\n", {}};
            }
            if (budyk::argon2_verify(pw, cfg.password_hash) != 0) {
                return budyk::HttpResponse{
                    401, "application/json",
                    "{\"error\":\"invalid credentials\"}\n", {}};
            }
            const std::string tok = sessions.create();
            if (tok.empty()) {
                return budyk::HttpResponse{
                    500, "application/json",
                    "{\"error\":\"entropy unavailable\"}\n", {}};
            }
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "application/json";
            r.body         = "{\"ok\":true}\n";
            r.extra_headers.push_back({"Set-Cookie",
                "budyk_session=" + tok + "; HttpOnly; Path=/; SameSite=Strict"});
            return r;
        }

        if (req.method == "POST" && req.path == "/api/auth/logout") {
            const std::string c = req.header("Cookie");
            if (!c.empty()) {
                const std::string tok = cookie_value(c, "budyk_session");
                if (!tok.empty()) sessions.revoke(tok);
            }
            budyk::HttpResponse r{200, "application/json", "{\"ok\":true}\n", {}};
            r.extra_headers.push_back({"Set-Cookie",
                "budyk_session=; HttpOnly; Path=/; Max-Age=0"});
            return r;
        }

        if (req.method == "GET" && req.path == "/api/samples") {
            if (!authed(req)) {
                return budyk::HttpResponse{
                    401, "application/json", "{\"error\":\"unauthenticated\"}\n", {}, {}};
            }
            std::vector<budyk::Sample> snap;
            {
                std::lock_guard<std::mutex> g(hot_mtx);
                snap.resize(hot.size());
                if (!snap.empty()) hot.dump(snap.data(), snap.size());
            }
            budyk::HttpResponse r;
            r.status       = 200;
            r.content_type = "application/json";
            r.body         = budyk::samples_to_json(snap.data(), snap.size());
            return r;
        }

        // Historical range query against the on-disk tier rings — lets
        // the SPA show hours/days, not just the 300-record hot buffer.
        //   GET /api/range?since=<ns>&until=<ns>&tier=<1|2|3>&limit=<n>
        //     since  — lower bound on timestamp_nanos (default 0 = all)
        //     until  — upper bound, 0 = now (default 0)
        //     tier   — 1 raw L3 / 2 1-min L2 / 3 5-min L1 (default 1)
        //     limit  — max samples returned, newest kept (default+cap 5000)
        {
            std::string rpath, rquery;
            split_target(req.path, &rpath, &rquery);
            if (req.method == "GET" && rpath == "/api/range") {
                if (!authed(req)) {
                    return budyk::HttpResponse{
                        401, "application/json",
                        "{\"error\":\"unauthenticated\"}\n", {}, {}};
                }
                constexpr uint64_t kMaxLimit = 5000;
                const uint64_t since = query_u64(rquery, "since", 0);
                const uint64_t until = query_u64(rquery, "until", 0);
                uint64_t tier        = query_u64(rquery, "tier",  1);
                uint64_t limit       = query_u64(rquery, "limit", kMaxLimit);
                if (tier < 1 || tier > 3) tier = 1;
                if (limit == 0 || limit > kMaxLimit) limit = kMaxLimit;

                std::vector<budyk::Sample> out;
                const int n = tm.query(static_cast<int>(tier), since, until,
                                       static_cast<size_t>(limit), &out);
                if (n < 0) {
                    return budyk::HttpResponse{
                        400, "application/json",
                        "{\"error\":\"bad range query\"}\n", {}, {}};
                }
                budyk::HttpResponse r;
                r.status       = 200;
                r.content_type = "application/json";
                r.body         = budyk::samples_to_json(out.data(), out.size());
                return r;
            }
        }

        if (req.path == "/api/ws") {
            if (!budyk::is_websocket_upgrade(req)) {
                return budyk::HttpResponse{
                    400, "text/plain", "expected websocket upgrade\n", {}, {}};
            }
            if (!authed(req)) {
                return budyk::HttpResponse{
                    401, "text/plain", "unauthenticated\n", {}, {}};
            }
            const std::string key = req.header("Sec-WebSocket-Key");
            const std::string handshake = budyk::ws_handshake_response(key);

            // Snapshot the hot buffer right now so the new client gets
            // history immediately on connect (catch-up frame).
            std::vector<budyk::Sample> snap;
            {
                std::lock_guard<std::mutex> g(hot_mtx);
                snap.resize(hot.size());
                if (!snap.empty()) hot.dump(snap.data(), snap.size());
            }
            std::string catchup = budyk::ws_text_frame(
                budyk::samples_to_json(snap.data(), snap.size()));

            budyk::HttpResponse r;
            r.hijack = [handshake, catchup, &ws](int fd) {
                ssize_t n1 = ::send(fd, handshake.data(), handshake.size(), MSG_NOSIGNAL);
                if (n1 != static_cast<ssize_t>(handshake.size())) {
                    ::close(fd);
                    return;
                }
                ssize_t n2 = ::send(fd, catchup.data(), catchup.size(), MSG_NOSIGNAL);
                if (n2 != static_cast<ssize_t>(catchup.size())) {
                    ::close(fd);
                    return;
                }
                ws.add(fd);
            };
            return r;
        }
        return budyk::HttpResponse{404, "text/plain", "not found\n", {}, {}};
    };
    if (http.start(cfg.listen_addr, cfg.listen_port, router) != 0) {
        std::fprintf(stderr,
            "budyk serve: HttpServer.start(%s:%d) failed — continuing without HTTP\n",
            cfg.listen_addr, cfg.listen_port);
    }

    std::fprintf(stderr,
        "budyk serve: started (config=%s, data_dir=%s, rules=%d, listen=%s:%d)\n",
        config_path, cfg.data_dir, engine.rule_count(),
        cfg.listen_addr, http.bound_port());

    budyk_cpu_ctx_c  cpu_ctx{};
    budyk_disk_ctx_c disk_ctx{};
    budyk_net_ctx_c  net_ctx{};

    // File watcher — init once before the loop, add every configured
    // path. Init failures (kernel unable to allocate inotify/kqueue)
    // disable the feature for this run; per-path add failures are
    // logged and the rest of the list still gets wired up.
    budyk::FileWatcher    file_watcher;
    budyk::FileWatchState file_state;
    bool                  fw_active = false;
    if (cfg.file_watch_enabled && !cfg.file_watch_paths.empty()) {
        if (file_watcher.init() == 0) {
            fw_active = true;
            for (const auto& p : cfg.file_watch_paths) {
                const int rc = file_watcher.add(p);
                if (rc < 0) {
                    std::fprintf(stderr,
                        "budyk serve: cannot watch '%s' (errno=%d)\n",
                        p.c_str(), -rc);
                }
            }
            std::fprintf(stderr,
                "budyk serve: file_watch active on %zu path(s)\n",
                file_watcher.count());
        } else {
            std::fprintf(stderr,
                "budyk serve: FileWatcher.init failed — file_watch disabled\n");
        }
    }

    // Flush rule state to disk every 60s as crash insurance (graceful
    // shutdown also saves below). A crash loses at most 60s of cooldown
    // decrement — acceptable, and erring toward "still in cooldown".
    time_t next_state_save = ::time(nullptr) + 60;

    while (!g_stop) {
        // SIGHUP latched in the handler — process it at a clean tick
        // boundary so an in-flight eval_tick / store can't race with
        // engine.shutdown(). One signal per cycle is plenty.
        if (g_reload) {
            g_reload = 0;
            reload_rules();
        }

        budyk::Sample s{};
        s.timestamp_nanos = now_realtime_ns();

        collect_one(&s, &cpu_ctx, &disk_ctx, &net_ctx);

        // File-watch drain — non-blocking poll so the tick cadence is
        // unchanged. apply() always clears the tampered set first, so
        // a tick with zero events drops `files[p].tampered` back to
        // false (one-shot semantics). Done before tm.store/hot.push so
        // the persisted + broadcast sample carries this tick's counts.
        if (fw_active) {
            std::vector<budyk::FileChangeEvent> events;
            const int n = file_watcher.poll(/*timeout_ms=*/0, &events);
            if (n < 0) {
                std::fprintf(stderr,
                    "budyk serve: file_watcher.poll failed (errno=%d)\n", -n);
            }
            file_state.apply(events);
            engine.set_file_state(file_state);
            s.file_watch.events_this_tick =
                static_cast<uint32_t>(file_state.tampered_this_tick.size());
            s.file_watch.watched_count =
                static_cast<uint32_t>(file_watcher.count());
            s.file_watch.present = true;
        }

        s.level = sched.tick(s);
        tm.store(s);
        {
            std::lock_guard<std::mutex> g(hot_mtx);
            hot.push(s);
        }

        engine.eval_tick(s);

        if (cfg.rules_persist_state) {
            const time_t now = ::time(nullptr);
            if (now >= next_state_save) {
                engine.save_state(state_path.c_str());
                next_state_save = now + 60;
            }
        }

        // Push the freshly-collected sample to every connected WS client.
        // Failed sends are evicted by the hub itself.
        ws.broadcast(budyk::samples_to_json(&s, 1));

        interruptible_sleep(level_interval_sec(s.level, cfg.scheduler));
    }

    std::fprintf(stderr, "budyk serve: shutting down\n");
    // Final flush so a graceful stop captures the exact cooldown state.
    if (cfg.rules_persist_state) {
        engine.save_state(state_path.c_str());
    }
    http.stop();
    ws.close_all();
    engine.shutdown();
    tm.close();
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
        std::printf("budyk 0.4.0\n");
        return 0;
    }

    if (std::strcmp(cmd, "serve") == 0) {
        return cmd_serve(argc, argv);
    }

    if (std::strcmp(cmd, "tui") == 0) {
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
        return budyk::tui_run(host, port) == 0 ? 0 : 1;
    }

    if (std::strcmp(cmd, "hash-password") == 0) {
        return cmd_hash_password();
    }

    if (std::strcmp(cmd, "suggest-rules") == 0) {
        return cmd_suggest_rules(argc, argv);
    }

    if (std::strcmp(cmd, "watch-files") == 0) {
        return cmd_watch_files(argc, argv);
    }

    std::fprintf(stderr, "budyk: unknown command '%s'\n", cmd);
    return 1;
}
