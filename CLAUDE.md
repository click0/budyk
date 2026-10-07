# CLAUDE.md — project context for Claude Code

## What is this project?

budyk (Ukrainian "будик" — alarm clock) is a lightweight, self-contained server
monitoring daemon for FreeBSD and Linux. Single static binary, no external
dependencies at runtime, no database. BSD-3-Clause license.

## Architecture overview

- **Language:** C++17 (everything except the collectors) + C (platform collectors)
- **Build:** CMake 3.22+; a static binary by default (`STATIC_LINK=ON`)
- **Collection:** 3-level adaptive model (L1 heartbeat / L2 watchful / L3 active)
  plus custom levels (name, interval, priority, Lua `when` condition, hold)
- **Rule engine:** embedded Lua 5.4, sandboxed `watch()` API, per-call
  instruction and memory limits
- **Storage:** one ring file per level (mmap'd header, `pwrite` records,
  CRC32C, level markers). No aggregation on disk yet: `TierAggregator` exists
  but is not wired in.
- **Web:** hand-written HTTP/1.1 + WebSocket (RFC 6455) server in `src/web`.
  No mongoose, no libwebsockets. The SPA is a string literal in
  `src/web/spa.cpp`.
- **TUI:** ncurses; polls `/api/samples` once a second
- **Alerts:** ntfy / Discord / Telegram / SMTP / Twilio through `curl(1)`,
  run from a worker thread

## Threads and I/O (how it actually runs)

There is no event-loop library (no libuv, kqueue or epoll). The daemon is:

- **Main thread:** the collector tick: collect → store → hot buffer → Lua
  rules → WebSocket broadcast → sleep until the next tick. The sleep is a
  `poll()` on a wake pipe, so an escalation (a client connecting, a rule
  calling `escalate()`) cuts it short.
- **HTTP thread:** one blocking accept-handle-close loop. Every connection
  is bounded by `HttpServer::set_io_timeout_ms` (5 s) so a stalled client
  cannot hold it. A WebSocket upgrade hands the fd to the hub.
- **WebSocket hub:** client fds are non-blocking. `broadcast()` drops a
  client whose send would block or writes a partial frame; `service()`,
  called by the main thread once per tick, answers pings, honours close
  frames and drops clients that hung up.
- **Worker threads** (`src/rules/worker.*`): one for alerts, one for
  `exec()`. Bounded FIFO of 256 jobs, started on first use, cancelled on
  shutdown after a grace period. Both survive a SIGHUP rules reload.
- **Children:** `exec_command()` forks with `/dev/null` stdio, a new
  process group, rlimits, a timeout and a cancel flag. Every descriptor the
  daemon opens is close-on-exec; `tests/smoke/serve.sh` checks that.
- **Shared state:** the hot buffer under a mutex; the ring files via
  `pread`/`pwrite` plus an atomic `write_idx` in the mmap'd header; the hub
  and the session store under their own mutexes.

## Directory structure

```
src/core/          — Sample struct, codec, json_text, endian.h, clock.h (C++, no I/O)
src/util/          — shared helpers that need I/O or a library: private temp
                     files (tmpfile), libyaml DOM lookups (yaml_dom)
src/collector/     — platform metric collection (plain C)
  freebsd/         — sysctl, devstat, kvm, getifaddrs
  linux/           — /proc, /sys parsers
src/scheduler/     — L1↔L2↔L3 (+ custom levels) tick scheduler, anomaly detection
src/hot_buffer/    — in-memory ring for WS catch-up (RAM-only)
src/storage/       — ring files (mmap header, pwrite, CRC32C), TierManager
src/rules/         — Lua 5.4 engine, sandbox, bindings, alerts, exec, freeze, Worker
src/security/      — file watcher (inotify / kqueue) feeding the `files` Lua global
src/ai/            — rule suggestions (Tier A: local stats; Tier B: LLM via curl)
src/web/           — HTTP + WebSocket server, auth (Argon2id), sessions, login
                     throttle, JSON, embedded SPA
src/tui/           — ncurses terminal UI
src/config/        — YAML config loader (libyaml)
src/daemon/        — the daemon: serve.cpp (loop, signals, reload, shutdown),
                     router.cpp (HTTP routes behind RouterDeps, testable without
                     a socket), collect.cpp (C collectors → Sample)
src/cli/           — the one-shot commands: hash-password, suggest-rules,
                     watch-files, tui
src/main.cpp       — dispatch only
tests/unit/        — one assert()-based executable per module (ctest)
tests/smoke/       — serve.sh (HTTP, WS, SIGHUP, fd check), crash.sh (SIGKILL +
                     restart), warm_grace.sh (hot buffer emptied after warm_grace)
addons/            — FreeBSD port + rc.d, systemd unit, Docker
docs/              — spec (en/uk), man page
```

## Build and test commands

```sh
cmake -B build                           # Release unless -DCMAKE_BUILD_TYPE says otherwise;
                                         # the host platform is detected,
                                         # -DBUDYK_PLATFORM=linux|freebsd only checks it matches
cmake --build build -j
ctest --test-dir build                   # unit tests
tests/smoke/serve.sh build/src/budyk     # daemon: endpoints, WS, SIGHUP, close-on-exec
tests/smoke/crash.sh build/src/budyk     # daemon: SIGKILL + restart, at most one record lost
tests/smoke/warm_grace.sh build/src/budyk  # daemon: hot buffer emptied after warm_grace
tests/check_whitespace.sh                # trailing blanks, tabs, CR, final newline
```

CI (see `.github/workflows/linux-build.yml`) adds `-DENABLE_WERROR=ON`, runs
the tests under ASan + UBSan (`-DSTATIC_LINK=OFF -DENABLE_SANITIZERS=ON`)
together with the smoke scripts, under ThreadSanitizer
(`-DSTATIC_LINK=OFF -DENABLE_TSAN=ON`) with the serve smoke test, runs
cppcheck, clang-tidy (checks in `.clang-tidy`) and
`tests/check_whitespace.sh`, gates line coverage of `core/` and `storage/`
at 85% (`tests/coverage_report.py`), and on FreeBSD 14.2 and 15.0 (clang)
builds and tests both the shared (port) and the static (release)
configuration and runs the smoke scripts on the static binary. Everything
must be green before a merge. Warnings are `-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion`
on every target; a new implicit sign or width change is a build error in CI
(GCC and clang both).

## Coding conventions

- SPDX license header in every file
- C++17, no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`). Lua raises
  errors with `luaL_error` (a longjmp): never hold a C++ object with a
  destructor across a call that may raise — see the pattern in
  `src/rules/lua_stdlib.cpp`.
- Collectors are plain C (no C++ in `src/collector/`)
- Errors via return codes. Two conventions exist today: `-errno` (collectors,
  file watcher, freeze, state files) and small negative ordinals (ring file,
  tier manager, HTTP server, exec, alerts, TUI client). The ordinal modules
  keep the reason alongside the code: `RingFile::describe(rc)` +
  `last_errno()`, `TierManager::last_error()`, `HttpServer::describe(rc)` +
  `last_errno()`, `config_load(..., &error)`. New code uses `-errno`. Every
  operator-facing message says what failed and includes `strerror`; never
  print a bare `rc=-3`.
- **Never block the collector tick.** No network I/O, no waiting on a child,
  no blocking send from the main thread: post the work to a `Worker` or use
  non-blocking I/O. Per-tick allocation of small strings and vectors is
  tolerated; unbounded growth is not.
- Numbers from a user (command line, config, rules) go through
  `parse_int_full()` (`src/core/parse_int.h`): whole string, in range, or
  an error that names the value. No `atoi()`; clang-tidy (`cert-err34-c`)
  rejects it outside the collectors.
- Other programs run through `exec_command()` with an argv, never a shell
  (`system`/`popen` are rejected by `cert-env33-c`); secrets go in 0600
  temp files from `write_private_tmp()`, not on the command line.
- Platform branching only through `#ifdef BUDYK_FREEBSD` / `#ifdef BUDYK_LINUX`
- Tests are `assert()`-based and built with `-UNDEBUG`, one
  `budyk_add_test()` line each in `tests/CMakeLists.txt`. New behaviour
  gets a unit test; a change in the daemon's behaviour gets a check in
  `tests/smoke/` too. When a test guards a fix, break the fix on purpose once
  and confirm the test fails — and confirm the broken build actually
  compiled under `-Werror`, or the old binary runs and the check proves
  nothing.
- Squash-merge; after the merge, confirm the tree on `main` is identical to
  the commit that was tested.

## Key design decisions (as implemented)

1. The collector tick runs on the main thread; HTTP on its own thread; slow
   work (alert delivery, `exec()`) on Worker threads with bounded queues.
2. Lua rules are sandboxed: no `io`/`os`/`load`/`loadfile`/`dofile`/`require`;
   the sample tables are read-only proxies, created once and updated in
   place (`src/rules/lua_bindings.cpp`);
   `exec()` is gated by `--enable-exec` or `rules.exec.enabled` plus an
   allowlist; every call into Lua is limited to `rules.limits.instructions`
   (default 1 000 000) and the engine to `rules.limits.memory_mb`
   (default 16). A runaway rule gets an error, not the daemon.
3. The hot buffer is RAM-only and never touches disk; it is emptied once no
   client has been connected for `hot_buffer.warm_grace` (`WarmGrace`).
4. Storage records carry an absolute timestamp, a level marker and a CRC32C;
   each level has its own ring; `write_idx` is advanced only after the
   `pwrite`, so a crash loses at most one record (`tests/smoke/crash.sh`).
5. `exec()` is asynchronous: it returns `{ queued = true }` and the outcome
   is logged. `exec(cmd, { timeout = s, wait = true })` is the inline form,
   capped at 60 s.
6. Slow WebSocket consumers are dropped, not waited on; a vanished client
   releases L3 on the next tick.
7. Failed logins are throttled per client address (429 + `Retry-After`);
   no password hash is computed for a throttled attempt.
8. The version string comes from `project(VERSION)` only (`BUDYK_VERSION`).

## Release procedure

1. Bump `project(budyk VERSION x.y.z)` in `CMakeLists.txt`, the `.TH` line in
   `docs/budyk.8` (date and version) and `DISTVERSION` in
   `addons/freebsd/Makefile`.
2. In `CHANGELOG.md` turn `[Unreleased]` into `[x.y.z] — YYYY-MM-DD` with a
   summary paragraph, add the tag link at the bottom, and open a new empty
   `[Unreleased]`. `release.yml` publishes exactly this section as the notes.
3. Open the PR; start a `release.yml` dry run (`workflow_dispatch` from the
   branch — it builds everything and skips publish); merge when all checks
   and the dry run are green.
4. The maintainer creates tag `vx.y.z` on `main`. The tag push builds static
   tarballs for linux-amd64, freebsd14.2-amd64 and freebsd15.0-amd64 (release
   and debug), checks every `.sha256` in one `sha256sum -c` run, and
   publishes the release.
5. Verify the published release: tag → commit, the body, 12 assets, checksums,
   `file` on every binary, `budyk version`, and the smoke scripts against the
   published Linux binary.

## Known gaps against the spec (`docs/budyk-spec-en.md`)

- No tier aggregation on disk; `TierAggregator` is unused. While a client
  holds L3, the L1 ring receives nothing.
- The metric set is roughly 40% of §2.1: no per-core CPU, per-device disk,
  per-interface network, TCP/socket counters, filesystem usage or process
  list.
- No `fsck`; damaged records are skipped on read.
- Every collector runs at every level; no per-level metric sets, no
  `collection.mode`.
- No L2 → L3 escalation on a sustained anomaly.
- Rule semantics that differ from §3.6: `cooldown` defaults to 0; a `nil`
  from `when()` resets the sustain counter; `action = { alert, exec(...) }`
  tables are rejected; `alert()` emits no WebSocket event.
- No Doxygen, no deb/rpm, no cross-compilation toolchains, FreeBSD 13 not in CI.

### Kept on purpose for later milestones (do not remove as dead code)

- `src/storage/tier_aggregator.*`: the L3 → L2/L1 fold (mean for
  percentages and load, last value or max/min for counters), tested but not
  wired into `TierManager`. 0.7.0 wires it in so the L1 ring keeps filling
  while a client holds L3.
- `src/core/metric_source.h`, `src/collector/collector.h`
  (`MetricSource`, `create_collector()`): the collector interface. `main.cpp`
  calls the C collectors directly today; the interface is for per-level
  metric sets (a minimal set at L1, the full set at L3).
- `ENABLE_EXEC` in `CMakeLists.txt`: a build-time gate for a binary with
  `exec()` compiled out (air-gapped or hardened hosts), on top of the runtime
  `--enable-exec` / `rules.exec.enabled`.

## Full technical specification

See `docs/budyk-spec-en.md` (English) and `docs/budyk-spec-uk.md` (Ukrainian).
