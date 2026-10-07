// SPDX-License-Identifier: BSD-3-Clause
#include "daemon/serve.h"

#include "config/config.h"
#include "core/clock.h"
#include "core/sample.h"
#include "core/sample_c.h"
#include "daemon/collect.h"
#include "daemon/router.h"
#include "hot_buffer/hot_buffer.h"
#include "rules/alert.h"
#include "rules/lua_engine.h"
#include "rules/yaml_compat.h"
#include "scheduler/scheduler.h"
#include "security/file_watcher.h"
#include "storage/tier_manager.h"
#include "web/json.h"
#include "web/login_limiter.h"
#include "web/server.h"
#include "web/session.h"
#include "web/ws_hub.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

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

extern "C" void budyk_serve_signal_handler(int sig) {
    if (sig == SIGHUP) g_reload = 1;
    else               g_stop   = 1;
}

namespace budyk {

namespace {


// Self-pipe that wakes the collection loop out of interruptible_sleep().
// A dashboard or TUI that connects while the loop sleeps at L1 (up to
// 5 min) must see L3 samples at once, not after the sleep ends. Written
// from the HTTP server thread; a non-blocking write() on a pipe is
// thread-safe, and a full pipe (EAGAIN) means a wake-up is pending anyway.
static int g_wake_pipe[2] = {-1, -1};

void wake_collection_loop() {
    if (g_wake_pipe[1] < 0) return;
    const char b = 1;
    (void)!::write(g_wake_pipe[1], &b, 1);
}

// Realtime ns of the last authenticated GET /api/samples. The TUI (and
// any other poller) has no WebSocket, so a poll within kPollerWindowNs
// counts as one connected client for the scheduler.
static std::atomic<uint64_t> g_last_poll_ns{0};
constexpr uint64_t kNsPerSec       = 1000000000;
constexpr uint64_t kPollerWindowNs = 5 * kNsPerSec;

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = budyk_serve_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                      // no SA_RESTART — let nanosleep return
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP,  &sa, nullptr);   // → rules reload

    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &ign, nullptr);
}

void warn_if_writable_by_others(const char* what, const char* path) {
    struct stat st{};
    if (path == nullptr || *path == '\0' || ::stat(path, &st) != 0) return;
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) == 0) return;
    std::fprintf(stderr,
        "budyk serve: WARNING: %s '%s' is writable by group or others (mode %04o); "
        "whoever can write it controls this daemon\n",
        what, path, static_cast<unsigned>(st.st_mode & 07777));
}

// Sleep for at most `ms` milliseconds, returning early when:
//   * a signal sets g_stop (shutdown) or g_reload (SIGHUP; the reload is
//     applied at the top of the next tick, so it no longer waits out an
//     L1 sleep);
//   * wake_collection_loop() is called (a client connected).
// poll() is interrupted by signals because the handlers are installed
// without SA_RESTART. Safe to call with ms <= 0 (no-op).
void interruptible_sleep(int ms) {
    if (ms <= 0 || g_stop || g_reload) return;
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t deadline_ms = static_cast<int64_t>(ts.tv_sec) * 1000 +
                                ts.tv_nsec / 1000000 +
                                static_cast<int64_t>(ms);
    for (;;) {
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        const int64_t now_ms = static_cast<int64_t>(ts.tv_sec) * 1000 +
                               ts.tv_nsec / 1000000;
        if (now_ms >= deadline_ms) return;

        struct pollfd pfd{g_wake_pipe[0], POLLIN, 0};
        const int rc = ::poll(&pfd, g_wake_pipe[0] >= 0 ? 1 : 0,
                              static_cast<int>(deadline_ms - now_ms));
        if (rc > 0) {
            char buf[64];
            while (::read(g_wake_pipe[0], buf, sizeof(buf)) > 0) {}
            return;
        }
        if (rc < 0 && errno != EINTR) return;
        if (g_stop || g_reload) return;
    }
}

} // namespace

int cmd_serve(int argc, char* argv[], const char* version) {
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
    std::string cfg_err;
    if (budyk::config_load(config_path, &cfg, &cfg_err) != 0) {
        std::fprintf(stderr,
            "budyk serve: failed to load config '%s': %s\n", config_path,
            cfg_err.c_str());
        return 1;
    }
    // CLI flags override config (operator intent on the command line wins).
    if (cli_enable_exec)   cfg.rules_enable_exec   = true;
    if (cli_enable_freeze) cfg.rules_enable_freeze = true;

    // Anyone who can write the rules file runs Lua as this daemon, and
    // anyone who can write the config decides what it does; say so when
    // group or others can.
    warn_if_writable_by_others("config", config_path);
    if (cfg.rules_path[0] != '\0') warn_if_writable_by_others("rules file", cfg.rules_path);
    // budyk speaks plain HTTP. With a password on and a non-loopback
    // listen address, the password and the session cookie cross the
    // network in the clear.
    if (cfg.auth_enabled && std::strncmp(cfg.listen_addr, "127.", 4) != 0) {
        std::fprintf(stderr,
            "budyk serve: WARNING: web.auth is enabled but listen=%s is not loopback; "
            "budyk serves plain HTTP, so the password and the session cookie travel "
            "unencrypted. Bind to 127.0.0.1 and put a TLS reverse proxy in front.\n",
            cfg.listen_addr);
    }

    install_signal_handlers();
    if (::pipe2(g_wake_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        // Not fatal: without it a new client waits for the current sleep
        // to end before the level steps up to L3.
        std::fprintf(stderr,
            "budyk serve: wake pipe unavailable (errno=%d); clients will "
            "switch the level to L3 on the next tick\n", errno);
        g_wake_pipe[0] = g_wake_pipe[1] = -1;
    }

    // Create data_dir if it is missing (one level, mode 0750 as the rc.d
    // script does), so a manual `budyk serve` on a fresh host works; a
    // missing parent is still an error, reported by TierManager below.
    {
        struct stat st{};
        if (::stat(cfg.data_dir, &st) != 0 && errno == ENOENT) {
            if (::mkdir(cfg.data_dir, 0750) == 0) {
                std::fprintf(stderr, "budyk serve: created data_dir '%s'\n", cfg.data_dir);
            } else if (errno != ENOENT) {
                std::fprintf(stderr, "budyk serve: cannot create data_dir '%s': %s\n",
                             cfg.data_dir, std::strerror(errno));
                return 1;
            }
        } else if (::stat(cfg.data_dir, &st) == 0 && !S_ISDIR(st.st_mode)) {
            std::fprintf(stderr, "budyk serve: data_dir '%s' is not a directory\n",
                         cfg.data_dir);
            return 1;
        }
    }

    budyk::TierManager tm;
    std::vector<budyk::LevelRingSpec> level_rings;
    level_rings.reserve(cfg.scheduler.custom_levels.size());
    for (const auto& lv : cfg.scheduler.custom_levels) {
        level_rings.push_back(budyk::LevelRingSpec{lv.id, lv.name, lv.storage_mb});
    }
    if (tm.init(cfg.data_dir,
                cfg.tier1_max_mb, cfg.tier2_max_mb, cfg.tier3_max_mb,
                level_rings) != 0) {
        // Which ring and why: a missing or unwritable data_dir, or a ring
        // whose size no longer matches its storage_mb / tierN_max_mb.
        std::fprintf(stderr,
            "budyk serve: TierManager.init('%s') failed: %s\n",
            cfg.data_dir, tm.last_error().c_str());
        return 1;
    }
    for (const auto& lv : cfg.scheduler.custom_levels) {
        std::fprintf(stderr,
            "budyk serve: level '%s' (id %u): every %d ms, priority %d%s%s\n",
            lv.name.c_str(), static_cast<unsigned>(lv.id), lv.interval_ms,
            lv.priority, lv.when.empty() ? "" : ", when: ",
            lv.when.c_str());
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
    // lua by extension), optionally register alert channels, then
    // optionally restore per-rule state from `restore_path` so cooldowns
    // survive. Factored into a lambda so the reload path can't drift away
    // from initial setup. Returns the engine.init rc (0 on success).
    //
    // register_channels MUST be false on reload: the AlertDispatcher is a
    // member of LuaEngine and shutdown() does NOT clear it, so the
    // channels registered at startup are still live after the swap. Alert
    // channels are config-level (not reloadable) anyway — re-adding them
    // would append duplicates (add_channel has no dedup), firing every
    // alert N+1 times after N reloads.
    auto setup_engine = [&](const char* restore_path, bool register_channels) -> int {
        if (engine.init(cfg.rules_enable_exec) != 0) return -1;
        engine.set_limits(static_cast<uint64_t>(cfg.rules_instruction_limit),
                          static_cast<size_t>(cfg.rules_memory_mb) << 20);
        if (!cfg.rules_exec_allow.empty()) {
            engine.set_exec_allowlist(cfg.rules_exec_allow);
        }
        engine.set_freeze_enabled(cfg.rules_enable_freeze);
        {
            // Names escalate() accepts, and the custom levels' `when`
            // conditions. Re-applied on reload: shutdown() drops both.
            std::vector<std::string> names = {"L1", "L2", "L3"};
            for (const auto& lv : cfg.scheduler.custom_levels) {
                names.push_back(lv.name);
                if (lv.when.empty()) continue;
                if (engine.add_level_condition(lv.id, lv.when) != 0) {
                    std::fprintf(stderr,
                        "budyk serve: level '%s': 'when' doesn't compile: %s; "
                        "the level can still be entered with escalate()\n",
                        lv.name.c_str(), engine.last_error().c_str());
                }
            }
            engine.set_level_names(std::move(names));
        }
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
            int         rc  = 0;
            std::string err;
            if (is_yaml) {
                std::string lua_src;
                if (budyk::yaml_rules_to_lua_file(cfg.rules_path, &lua_src) != 0) {
                    rc  = -1;
                    err = "YAML parse error";
                } else if ((rc = engine.load_string(lua_src.c_str())) != 0) {
                    err = engine.last_error();
                }
            } else if ((rc = engine.load_file(cfg.rules_path)) != 0) {
                err = engine.last_error();
            }
            if (rc != 0) {
                // Loading stops at the first error; rules defined above it
                // stay registered, so report how many are actually active.
                std::fprintf(stderr,
                    "budyk serve: rules file '%s' failed to load: %s "
                    "(%d rule(s) before the error remain active)\n",
                    cfg.rules_path, err.c_str(), engine.rule_count());
            }
        }
        // Restore per-rule cooldown / fire counters. Must come after
        // load_file/load_string (matches saved entries to rules by name).
        if (restore_path != nullptr) {
            engine.load_state(restore_path);
        }
        // Register configured alert channels so rules can call
        // alert(name, severity, message) and reach them. Startup only —
        // see register_channels note above: the dispatcher survives the
        // reload swap, so re-adding here would duplicate every channel.
        if (register_channels) {
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
    if (setup_engine(state_path.empty() ? nullptr : state_path.c_str(),
                     /*register_channels=*/true) != 0) {
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
        // If the snapshot can't be written (disk full, bad perms) the
        // restore below reads a missing/stale file and silently zeroes
        // every cooldown — warn so an operator can see why rules that
        // were mid-cooldown suddenly re-fire after the reload.
        if (engine.save_state(rp.c_str()) != 0) {
            std::fprintf(stderr,
                "budyk serve: SIGHUP — could not snapshot rule state to '%s'; "
                "cooldowns may reset\n", rp.c_str());
        }
        engine.shutdown();
        if (setup_engine(rp.c_str(), /*register_channels=*/false) != 0) {
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
    // Login throttle. Used only from the HTTP thread (the router), so
    // it needs no lock.
    budyk::LoginLimiter  login_limiter(cfg.auth_max_login_failures,
                                       cfg.auth_login_window_sec);
    budyk::WebSocketHub  ws;

    // Restore logged-in sessions across restarts. The file holds live
    // bearer tokens (mode 0600); load drops any already past their TTL.
    if (cfg.auth_persist_sessions) {
        const std::string session_path =
            std::string(cfg.data_dir) + "/sessions.tsv";
        sessions.load(session_path.c_str());
        sessions.set_persist_path(session_path);   // autosave on mutation
    }


    // The HTTP thread only reads sched's level table (names, intervals,
    // priorities), which is fixed at construction, never its live state.
    RouterDeps deps;
    deps.cfg              = &cfg;
    deps.hot              = &hot;
    deps.hot_mtx          = &hot_mtx;
    deps.tm               = &tm;
    deps.sessions         = &sessions;
    deps.limiter          = &login_limiter;
    deps.ws               = &ws;
    deps.sched            = &sched;
    deps.last_poll_ns     = &g_last_poll_ns;
    deps.poller_window_ns = kPollerWindowNs;
    deps.wake             = [] { wake_collection_loop(); };
    deps.version          = version;
    const HttpHandler router = make_router(deps);
    if (const int hrc = http.start(cfg.listen_addr, cfg.listen_port, router); hrc != 0) {
        std::fprintf(stderr,
            "budyk serve: HttpServer.start(%s:%d) failed: %s%s%s — continuing without HTTP\n",
            cfg.listen_addr, cfg.listen_port, budyk::HttpServer::describe(hrc),
            http.last_errno() != 0 ? ": " : "",
            http.last_errno() != 0 ? std::strerror(http.last_errno()) : "");
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
    // Drop expired web sessions on the same cadence. verify() and
    // create() also purge, but a quiet daemon sees neither for hours,
    // and the table (and sessions.tsv) would hold every past login.
    time_t next_session_purge = next_state_save;

    // hot_buffer.warm_grace: empty the hot buffer once no client has been
    // connected for this long (see WarmGrace).
    budyk::WarmGrace warm(static_cast<uint64_t>(cfg.hot_buffer_warm_grace) * kNsPerSec);

    // Hold time per custom level id, and a reusable list of the levels
    // whose `when` holds on the current sample.
    uint64_t level_hold_ns[budyk::kMaxLevelId + 1] = {};
    for (const auto& lv : cfg.scheduler.custom_levels) {
        level_hold_ns[lv.id] = static_cast<uint64_t>(lv.hold_sec) * kNsPerSec;
    }
    std::vector<uint8_t> level_hits;

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
                    "budyk serve: file_watcher.poll failed: %s\n", std::strerror(-n));
            }
            file_state.apply(events);
            engine.set_file_state(file_state);
            s.file_watch.events_this_tick =
                static_cast<uint32_t>(file_state.tampered_this_tick.size());
            s.file_watch.watched_count =
                static_cast<uint32_t>(file_watcher.count());
            s.file_watch.present = true;
        }

        // Clients that hold the level at L3: every open WebSocket (the
        // dashboard) plus one for a recent /api/samples poller (the TUI).
        // service() first: it answers pings and drops clients that have
        // closed or stopped reading, so a vanished dashboard releases
        // L3 on this tick rather than when a send finally fails.
        {
            ws.service();
            int clients = static_cast<int>(ws.size());
            const uint64_t last_poll = g_last_poll_ns.load();
            if (last_poll != 0 && s.timestamp_nanos >= last_poll &&
                s.timestamp_nanos - last_poll < kPollerWindowNs) {
                ++clients;
            }
            sched.set_client_count(clients);
            if (warm.should_reset(clients, s.timestamp_nanos)) {
                std::lock_guard<std::mutex> g(hot_mtx);
                hot.reset();
            }
        }
        // Custom levels whose `when` holds on this sample stay requested
        // for their hold time (0 = this tick only).
        engine.eval_level_conditions(s, &level_hits);
        for (uint8_t id : level_hits) {
            sched.request(static_cast<budyk::Level>(id),
                          s.timestamp_nanos + level_hold_ns[id]);
        }
        s.level = sched.tick(s);
        tm.store(s);
        {
            std::lock_guard<std::mutex> g(hot_mtx);
            hot.push(s);
        }

        engine.eval_tick(s);
        for (const auto& e : engine.take_escalations()) {
            sched.request_by_name(e.level, now_realtime_ns() +
                static_cast<uint64_t>(e.seconds) * kNsPerSec);
        }

        {
            const time_t now = ::time(nullptr);
            if (cfg.rules_persist_state && now >= next_state_save) {
                engine.save_state(state_path.c_str());
                next_state_save = now + 60;
            }
            if (now >= next_session_purge) {
                sessions.purge_expired();
                next_session_purge = now + 60;
            }
        }

        // Push the freshly-collected sample to every connected WS client.
        // Failed sends are evicted by the hub itself. Nothing to build
        // when nobody is connected, which is the usual state at L1.
        if (ws.size() > 0) ws.broadcast(budyk::samples_to_json(&s, 1));

        // Re-select after the rules ran, so an escalate() takes effect for
        // this sleep, not only from the next tick. The sample's own level
        // holds until the next tick re-checks its condition.
        const budyk::Level next = sched.higher(s.level, sched.select(now_realtime_ns()));
        // Wake for the warm-grace reset too, or an L1 sleep of minutes
        // would leave stale samples for the next client's catch-up.
        int sleep_ms = sched.interval_ms(next);
        const uint64_t due_ns = warm.ns_until_due(now_realtime_ns());
        if (due_ns != UINT64_MAX) {
            const uint64_t due_ms = due_ns / 1000000 + 1;
            if (due_ms < static_cast<uint64_t>(sleep_ms)) sleep_ms = static_cast<int>(due_ms);
        }
        interruptible_sleep(sleep_ms);
    }

    std::fprintf(stderr, "budyk serve: shutting down\n");
    // Final flush so a graceful stop captures the exact cooldown state.
    if (cfg.rules_persist_state) {
        engine.save_state(state_path.c_str());
    }
    http.stop();
    ws.close_all();
    // Queued alerts get a few seconds to go out and a running exec()
    // child a moment to finish; then the rest is cancelled, so a stuck
    // channel or command cannot hold the stop.
    engine.alerts().stop(5000);
    engine.exec_worker().stop(2000);
    engine.shutdown();
    tm.close();
    return 0;
}

} // namespace budyk
