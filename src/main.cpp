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
#include "core/sample_c.h"
#include "hot_buffer/hot_buffer.h"
#include "rules/lua_engine.h"
#include "scheduler/scheduler.h"
#include "storage/codec.h"
#include "storage/ring_file.h"
#include "storage/tier_manager.h"
#include "web/auth.h"
#include "web/server.h"

#include <atomic>
#include <cerrno>
#include <csignal>
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

static volatile std::sig_atomic_t g_stop = 0;

extern "C" void serve_signal_handler(int /*sig*/) { g_stop = 1; }

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = serve_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                      // no SA_RESTART — let nanosleep return
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

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
#elif defined(BUDYK_FREEBSD)
    budyk_collect_cpu_freebsd    (cpu_ctx, &c);
    budyk_collect_memory_freebsd (&c);
    budyk_collect_uptime_freebsd (&c);
    budyk_collect_load_freebsd   (&c);
    budyk_collect_disk_freebsd   (disk_ctx, &c);
    budyk_collect_network_freebsd(net_ctx,  &c);
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
    s->uptime_seconds         = c.uptime_seconds;
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

int cmd_serve(int argc, char* argv[]) {
    const char* config_path = "/usr/local/etc/budyk/config.yaml";
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
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

    install_signal_handlers();

    budyk::TierManager tm;
    if (tm.init(cfg.data_dir,
                cfg.tier1_max_mb, cfg.tier2_max_mb, cfg.tier3_max_mb) != 0) {
        std::fprintf(stderr,
            "budyk serve: TierManager.init('%s') failed\n", cfg.data_dir);
        return 1;
    }

    budyk::HotBuffer hot(static_cast<size_t>(cfg.hot_buffer_capacity));

    budyk::Scheduler sched(cfg.scheduler);

    budyk::LuaEngine engine;
    if (engine.init(cfg.rules_enable_exec) != 0) {
        std::fprintf(stderr, "budyk serve: LuaEngine.init failed\n");
        tm.close();
        return 1;
    }
    if (!cfg.rules_exec_allow.empty()) {
        engine.set_exec_allowlist(cfg.rules_exec_allow);
    }
    if (cfg.rules_path[0] != '\0') {
        if (engine.load_file(cfg.rules_path) != 0) {
            std::fprintf(stderr,
                "budyk serve: rules file '%s' failed to load — continuing without rules\n",
                cfg.rules_path);
        }
    }

    budyk::HttpServer http;
    if (http.start(cfg.listen_addr, cfg.listen_port,
                   [&cfg](const budyk::HttpRequest& req) {
                       if (req.method == "GET" && req.path == "/api/health") {
                           budyk::HttpResponse r;
                           r.status       = 200;
                           r.content_type = "application/json";
                           r.body =
                               "{\"status\":\"ok\","
                               "\"version\":\"0.2.0\","
                               "\"data_dir\":\"" + std::string(cfg.data_dir) + "\"}\n";
                           return r;
                       }
                       return budyk::HttpResponse{404, "text/plain", "not found\n"};
                   }) != 0) {
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

    while (!g_stop) {
        budyk::Sample s{};
        s.timestamp_nanos = now_realtime_ns();

        collect_one(&s, &cpu_ctx, &disk_ctx, &net_ctx);

        s.level = sched.tick(s);
        tm.store(s);
        hot.push(s);
        engine.eval_tick(s);

        interruptible_sleep(level_interval_sec(s.level, cfg.scheduler));
    }

    std::fprintf(stderr, "budyk serve: shutting down\n");
    http.stop();
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
        std::printf("budyk 0.2.0\n");
        return 0;
    }

    if (std::strcmp(cmd, "serve") == 0) {
        return cmd_serve(argc, argv);
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
