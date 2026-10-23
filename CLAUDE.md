# CLAUDE.md — project context for Claude Code

## What is this project?

budyk (Ukrainian "будик" — alarm clock) is a lightweight, self-contained server
monitoring daemon for FreeBSD and Linux. Single static binary, no external
dependencies at runtime, no database. BSD-3-Clause license.

## Architecture overview

- **Language:** C++17 (everything except the collectors) + C (platform collectors)
- **Build:** CMake 3.22+; a static binary by default (`STATIC_LINK=ON`)
- **Collection:** 3-level adaptive model (L1 heartbeat / L2 watchful / L3 active)
  plus custom levels (name, interval, priority, Lua `when` condition, hold);
  each level reads its own `MetricSet` (`collection.<level>.metrics`:
  minimal / standard / full or a list; `collect_one()` runs only those
  collectors), and `collection.mode` is adaptive / always / on-demand.
  `Sample::groups` says what a sample has; a rate's first read in a row is
  a baseline, not in `groups` (`Sample::baseline`). Rules skip what their
  sample lacks (`lua_bindings.cpp`: the proxies raise, the engine skips)
- **Rule engine:** embedded Lua 5.4, sandboxed `watch()` API, per-call
  instruction and memory limits
- **Metrics:** fixed totals in `Sample` (persisted); per-entity detail
  (`budyk_detail_c`: each core, disk, interface, filesystem, and the top
  processes from the tracker in `src/collector/detail.c`) collected by
  the same C collectors through their `_ex` variants, served live (the
  `"detail"` block of a WebSocket frame and of `/api/samples`, the rules'
  `cpu.cores` / `disk.devices` / `net.interfaces` / `fs.mounts` /
  `proc.top`), never stored. JSON strings are made valid UTF-8
  (`json_escape`): process names and mount points are anyone's bytes.
  The disk and network totals are each device's own delta summed
  (`budyk_counted_add()`): a device that comes or goes moves nothing.
  A collector that fails leaves its groups out of the sample (logged when
  it starts failing), and a tick that reads nothing makes the next read
  a baseline. Filesystems are read from `/proc/self/mountinfo`, read-only
  by the filesystem's own options (systemd's ProtectSystem=strict makes
  every mount look read-only).
- **Storage:** one ring file per level (mmap'd header, `pwrite` records,
  CRC32C, level markers). A new ring's header goes in with `pwrite`
  before it is mapped (a store through the mapping into the sparse
  file's hole is SIGBUS on a full disk); a ring of the right size with an
  all-zero header is made anew; a store that fails is logged once with
  its reason (`TierManager::store_error()`). While L3 or a custom level collects,
  `TierManager::store()` also folds its samples (`TierAggregator`) into one
  L2 record per L2 interval and one L1 record per L1 interval, flagged
  `folded` in the record header; `query_all()` (`/api/range?level=all`)
  shows each stretch once, at the finest level still on disk.
- **Web:** hand-written HTTP/1.1 + WebSocket (RFC 6455) server in `src/web`.
  No mongoose, no libwebsockets. The SPA is a string literal in
  `src/web/spa.cpp`.
- **TUI:** ncurses; polls `/api/samples` once a second
- **Alerts:** every alert is logged, queued by `LuaEngine::raise_alert()`
  for the serve loop (WebSocket frame `{"alerts":[...]}`, the last 100 for
  `/api/alerts`) and sent to ntfy / Discord / Telegram / SMTP / Twilio
  through `curl(1)` from a worker thread
- **Rules (spec §3.6):** `cooldown` defaults to `for_ticks`; `when()`
  returning nil skips the tick and keeps the streak, returning nothing is
  false; `action` may be a list (`{ alert, escalate, exec(...) }`); `exec()`
  in the rules file itself returns the command as an action instead of
  running it (`LuaEngine::loading()`)

## Threads and I/O (how it actually runs)

There is no event-loop library (no libuv, kqueue or epoll). The daemon is:

- **Main thread:** the collector tick: collect → store → hot buffer → Lua
  rules → WebSocket broadcast → sleep until the next tick. The sleep is a
  `poll()` on a wake pipe, so an escalation (a client connecting, a rule
  calling `escalate()`) cuts it short, though to no less than 250 ms after
  the tick (`kMinWakeGapMs`: a rate over a few ms is noise). A SIGHUP is
  applied inside the sleep, which goes on. The new rules file is loaded
  into a scratch engine first, and one that does not load leaves the
  rules as they were. In on-demand mode a tick whose level is L1 or L2
  only sleeps, and nothing is stored.
- **HTTP thread:** one blocking accept-handle-close loop. Every connection
  is bounded by `HttpServer::set_io_timeout_ms` (5 s) so a stalled client
  cannot hold it: the request by that, the response by that plus its size
  at `set_min_send_rate` (256 KiB/s), so a slow reader is cut off. An
  accept() that fails (EMFILE, ECONNABORTED) is retried, logged once,
  never the end of the loop. A WebSocket upgrade hands the fd to the hub:
  same origin only (Origin against Host / X-Forwarded-Host), at most
  `WebSocketHub::kMaxClients` (64).
- **WebSocket hub:** client fds are non-blocking. `broadcast()` drops a
  client whose send would block or writes a partial frame; `service()`,
  called by the main thread once per tick, answers pings, honours close
  frames and drops clients that hung up; with auth on, `drop_sessions()`
  then closes (1008) every client whose session ended (logout, expiry).
- **Worker threads** (`src/rules/worker.*`): one for alerts, one for
  `exec()`. Bounded FIFO of 256 jobs, started on first use, cancelled on
  shutdown after a grace period. Both survive a SIGHUP rules reload.
- **Children:** `exec_command()` forks with `/dev/null` stdio, a new
  process group, rlimits, a timeout and a cancel flag. Every descriptor the
  daemon opens is close-on-exec; `tests/smoke/serve.sh` checks that.
- **Shared state:** the hot buffer under a mutex; the ring files via
  `pread`/`pwrite` plus the write index, an atomic in `RingFile` (`widx_`)
  that the writer also copies into the mmap'd header after each `pwrite`
  (the header's field is not 8-byte aligned: an atomic there is SIGBUS on
  arm64); the hub and the session store under their own mutexes.

## Directory structure

```
src/core/          — Sample struct, codec, metric_set, json_text, endian.h, clock.h
                     (C++, no I/O)
src/util/          — shared helpers that need I/O or a library: the data_dir
                     lock (dir_lock), private temp files (tmpfile), libyaml
                     DOM lookups (yaml_dom)
src/collector/     — platform metric collection (plain C)
  freebsd/         — sysctl, devstat, kvm, getifaddrs
  linux/           — /proc, /sys parsers
src/scheduler/     — L1↔L2↔L3 (+ custom levels) tick scheduler, anomaly detection
src/hot_buffer/    — in-memory ring for WS catch-up (RAM-only)
src/storage/       — ring files (mmap header, pwrite, CRC32C), TierManager,
                     TierAggregator (the L2/L1 folds), ring_check (fsck and
                     the start-up recovery of unindexed records)
src/rules/         — Lua 5.4 engine, sandbox, bindings, alerts, exec, freeze, Worker;
                     lua_pattern.c: Lua's own string patterns, step-counted (MIT)
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
                     watch-files, tui, fsck (fsck.cpp)
src/main.cpp       — dispatch only
tests/unit/        — one assert()-based executable per module (ctest)
tests/smoke/       — serve.sh (HTTP, WS, SIGHUP, fd check), crash.sh (SIGKILL +
                     restart), warm_grace.sh (hot buffer emptied after warm_grace),
                     escalate.sh (a sustained anomaly escalates L2 to L3),
                     fsck.sh (fsck, recovery after a crash, the data_dir lock),
                     upgrade.sh (rings of an older release converted on start),
                     modes.sh (per-level metric sets, always / on-demand),
                     resources.sh (spec §5 items 2-6: RSS, CPU, disk, L1 -> L3,
                     catch-up, binary size; release builds only),
                     auth.sh (login, the WebSocket ends with its session, a
                     cross-origin WebSocket refused),
                     crosscheck.sh (the readings against free, swapinfo, uptime,
                     ps, ss / netstat, df, /sys or ifconfig and kern.disks, a
                     busy loop and a 64 MB write)
addons/            — FreeBSD port + rc.d, systemd unit, Docker; the .deb and
                     .rpm (addons/linux/mkpkg.sh, test-packages.sh)
cmake/toolchains/  — cross-compiling for FreeBSD on Linux: freebsd-sysroot.sh
                     (base.txz + packages, checked), freebsd-{amd64,aarch64}.cmake
cmake/docs.cmake   — the `docs` target: Doxygen over the headers in src/
docs/              — spec (en/uk), man page
```

## Build and test commands

```sh
cmake -B build                           # Release unless -DCMAKE_BUILD_TYPE says otherwise;
                                         # the platform is CMAKE_SYSTEM_NAME (host or toolchain),
                                         # -DBUDYK_PLATFORM=linux|freebsd only checks it matches
cmake --build build -j
ctest --test-dir build                   # unit tests
tests/smoke/serve.sh build/src/budyk     # daemon: endpoints, WS, SIGHUP, close-on-exec
tests/smoke/crash.sh build/src/budyk     # daemon: SIGKILL + restart, at most one record lost
tests/smoke/warm_grace.sh build/src/budyk  # daemon: hot buffer emptied after warm_grace
tests/smoke/escalate.sh build/src/budyk  # daemon: a sustained anomaly escalates L2 to L3
tests/smoke/fsck.sh build/src/budyk      # fsck, start-up recovery, the data_dir lock
tests/smoke/upgrade.sh build/src/budyk OLD/budyk  # rings of an older release converted
tests/smoke/modes.sh build/src/budyk     # metric sets per level, always / on-demand
tests/smoke/auth.sh build/src/budyk      # login, sessions, the WebSocket's origin and end
tests/smoke/resources.sh build/src/budyk # spec §5 figures (a release build; ~2 min)
tests/smoke/crosscheck.sh build/src/budyk  # the readings against the system's own tools
tests/check_whitespace.sh                # trailing blanks, tabs, CR, final newline
cmake --build build --target docs        # API reference (Doxygen), any warning fails it
cmake/toolchains/freebsd-sysroot.sh 14.4 amd64 SYSROOT   # then, on Linux:
cmake -B build-fbsd --toolchain cmake/toolchains/freebsd-amd64.cmake -DFREEBSD_SYSROOT=SYSROOT
```

CI (see `.github/workflows/linux-build.yml`) adds `-DENABLE_WERROR=ON`, builds
and tests the .deb and .rpm (`addons/linux/test-packages.sh`), holds the
static builds (Linux, FreeBSD 14.4 and 15.1) to the spec's resource figures
(`tests/smoke/resources.sh`), runs
the tests under ASan + UBSan (`-DSTATIC_LINK=OFF -DENABLE_SANITIZERS=ON`)
together with the smoke scripts, under ThreadSanitizer
(`-DSTATIC_LINK=OFF -DENABLE_TSAN=ON`) with the serve smoke test, runs
cppcheck, clang-tidy (checks in `.clang-tidy`), the Doxygen reference
(`--target docs`) and `tests/check_whitespace.sh`, gates line coverage of `core/` and `storage/`
at 85% (`tests/coverage_report.py`), cross-compiles for FreeBSD 14.4 amd64
and aarch64, runs clang-tidy on the FreeBSD side of the code and the aarch64
unit tests and smoke scripts in an emulated arm64 FreeBSD 14.4, and on
FreeBSD 14.4 and 15.1 (clang; the oldest supported release of each branch)
builds and tests both the shared (port) and the static (release)
configuration and runs the smoke scripts on the static binary (on 15.1 also
on the cross-compiled amd64 one). Everything
must be green before a merge. Warnings are `-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion`
on every target, plus `-Wcast-align=strict` with GCC; a new implicit sign or
width change, or a cast to a more strictly aligned pointer, is a build error
in CI.

## Coding conventions

- SPDX license header in every file
- In a header, the comment that documents a declaration is Doxygen's:
  `///` above it, `///<` after a member on its line (`/** */`, `/**< */` in
  the C header `sample_c.h`), a file's own description under `/// @file`.
  Comments in `.c`/`.cpp` files and inside function bodies stay `//`. Put
  `<placeholders>`, `\n` and the like in backticks: Doxygen otherwise reads
  them as HTML tags and commands, and its warnings fail CI.
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
  rejects it outside the collectors. In the config, durations take a unit
  or none (`apply_seconds`: 300, 300s, 5m, 1h, 1d), and a value that does
  not read is reported under its full name with the default kept.
- Every descriptor is close-on-exec, short-lived ones too: `fopen(path,
  "re")` / `"we"`, `O_CLOEXEC`, `SOCK_CLOEXEC`, `mkostemp(..., O_CLOEXEC)`
  (worker threads fork while the main thread has a file open).
  `tests/smoke/serve.sh` watches the daemon's descriptors for 3 s.
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
   place (`src/rules/lua_bindings.cpp`), bound in a protected call with
   raw sets (a failed bind skips the tick's rules; nothing after a
   failed call may allocate on the Lua heap outside one);
   library work in C is charged to the instruction limit too
   (`LuaEngine::charge()`): `src/rules/lua_pattern.c` is Lua 5.4.7's
   own pattern matcher (MIT, kept as Lua writes it, the changes marked
   "budyk") with a step count, `table.move` is charged, past the limit
   the hook raises on every instruction (no `pcall` carries on),
   `xpcall` runs its handler after the call and `setmetatable` refuses
   `__gc` (Lua runs both with hooks off);
   `exec()` is gated by `--enable-exec` or `rules.exec.enabled` plus an
   allowlist; every call into Lua is limited to `rules.limits.instructions`
   (default 1 000 000) and the engine to `rules.limits.memory_mb`
   (default 16). A runaway rule gets an error, not the daemon.
3. The hot buffer is RAM-only and never touches disk; it is emptied once no
   client has been connected for `hot_buffer.warm_grace` (`WarmGrace`).
4. Storage records carry an absolute timestamp, a level marker, a flags
   byte (bit 0: folded) and a CRC32C; each level has its own ring;
   `write_idx` is advanced only after the `pwrite`. A codec version only
   appends fields; a ring of smaller (older) records is converted on start
   (`convert_ring()`, tests/smoke/upgrade.sh against v0.6.4 and v0.7.0;
   codec v9, 542-byte records, since ICMP). On start
   `TierManager::init()` also counts in a record that reached its slot but
   not the index (`count_unindexed()`), so a daemon crash loses no stored
   record (`tests/smoke/crash.sh`, `fsck.sh`). `budyk serve` holds an
   flock on `<data_dir>/budyk.lock`: one daemon per data_dir. The L2 and
   L1 rings get folded records while a finer level runs (spec §3.4.1); a
   window ends after its interval, when the level drops to the ring's
   own, or on shutdown — a SIGKILL loses the open windows, not the raw
   samples.
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
4. The maintainer publishes: **Actions → Release binaries → Run workflow**
   on `main` with `tag` = `vx.y.z` (the workflow checks CMakeLists.txt and
   CHANGELOG.md, builds, and creates the tag and the release itself), or a
   push of tag `vx.y.z` on `main`. Never a release made by hand on GitHub
   first: the workflow's token may not update it, so its files are never
   attached (v0.6.5). Either way the run builds static tarballs, release and debug, for
   linux-amd64, freebsd14.4-amd64 and freebsd15.1-amd64 (built in VMs) and
   freebsd14.4-aarch64 (cross-compiled, smoke-tested in an emulated arm64
   VM), and the Linux .deb and .rpm (installed, run and removed first),
   checks every `.sha256` in one `sha256sum -c` run, and publishes the
   release.
5. Verify the published release: tag → commit, the body, 20 assets, checksums,
   `file` on every binary, `budyk version`, and the smoke scripts against the
   published Linux binary.

## Known gaps against the spec (`docs/budyk-spec-en.md`)

- The per-entity detail (cores, disks, interfaces, filesystems, top
  processes) is live only: not in the rings, so not in the history.
- FreeBSD 13 not in CI.

### Kept on purpose for later milestones (do not remove as dead code)

- `src/core/metric_source.h`, `src/collector/collector.h`
  (`MetricSource`, `create_collector()`): the collector interface, unused.
  Per-level metric sets did not need it: `collect_one()` calls the C
  collectors directly and leaves out those the level's `MetricSet` does
  not ask for.
- `ENABLE_EXEC` in `CMakeLists.txt`: a build-time gate for a binary with
  `exec()` compiled out (air-gapped or hardened hosts), on top of the runtime
  `--enable-exec` / `rules.exec.enabled`.

## Full technical specification

See `docs/budyk-spec-en.md` (English) and `docs/budyk-spec-uk.md` (Ukrainian).
