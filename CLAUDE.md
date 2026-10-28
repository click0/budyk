# CLAUDE.md — project context for Claude Code

## What is this project?

budyk (Ukrainian "будик" — alarm clock) is a lightweight, self-contained server
monitoring daemon for FreeBSD and Linux. Single static binary, no external
dependencies at runtime, no database. BSD-3-Clause license.

## The spec and this file

The design is in `docs/budyk-spec-en.md` (Ukrainian: `docs/budyk-spec-uk.md`,
same section numbers): the adaptive L1/L2/L3 model (§3.4.1), the hot buffer
(§3.4.2), irregular density in the rings (§3.4.3), the `collection:` config
(§3.4.4), the ring format (§3.5), the Lua rules: syntax, sandbox, pipeline,
nil skips a tick (§3.6), AI suggestions (§3.7), the resource figures (§5).
Read it there; this file does not repeat it.

The spec was written before the code. Where they differ, the code wins, and
this file says how it is actually built. The spec's proposals that were
**not** followed: the project layout (§3.1; see the tree below), libuv /
kqueue / epoll, mongoose / libwebsockets, msgpack, cJSON (§3.2), an event
loop with all I/O non-blocking (§3.3 item 6; see Threads and I/O),
`MetricSource` as the extension point (§3.3 item 2; kept unused, see the
end), `__FreeBSD__` / `__linux__` branching (§3.3 item 1; `BUDYK_FREEBSD` /
`BUDYK_LINUX`), `write_idx` updated atomically in the mmap'd header (§3.5;
SIGBUS on arm64, see Shared state), and a 10 ms time limit per rule (§3.6.3;
an instruction count, see the rules decision).

## Architecture: beyond the spec

- **Language and build:** C++17 + plain C collectors, CMake 3.22+, static by
  default (`STATIC_LINK=ON`); spec §3.3 items 1, 7, 8.
- **Collection:** besides L1/L2/L3, custom levels (name, interval, priority,
  Lua `when` condition, hold); each level reads its own `MetricSet`
  (`collection.<level>.metrics`: minimal / standard / full or a list;
  `collect_one()` runs only those collectors). `Sample::groups` says what a
  sample has; a rate's first read in a row is a baseline, not in `groups`
  (`Sample::baseline`). Rules skip what their sample lacks
  (`lua_bindings.cpp`: the proxies raise, the engine skips).
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
  The network totals count only the devices' interfaces (Linux: one
  with `/sys/class/net/<if>/device`, not enslaved to another device as
  an SR-IOV VF is; FreeBSD: Ethernet or 802.11 and its driver, which a
  rename leaves, not a cloner's, `budyk_freebsd_iface_physical()`), or
  every one when there is none (budyk in a container): a veth's,
  bridge's or epair's traffic crosses a device too.
  The lists hold `BUDYK_MAX_ENTITIES` (256) of each, the totals follow
  `BUDYK_MAX_COUNTED` (1024) devices of each kind; past either,
  `collect_one()` logs it once (`CollectContext::cut`).
  A collector that fails leaves its groups out of the sample (logged when
  it starts failing), and a tick that reads nothing makes the next read
  a baseline. Filesystems are read from `/proc/self/mountinfo`, read-only
  by the filesystem's own options (systemd's ProtectSystem=strict makes
  every mount look read-only).
- **Storage:** one ring file per level, records in a codec of our own. A
  new ring's header goes in with `pwrite` before it is mapped (a store
  through the mapping into the sparse file's hole is SIGBUS on a full
  disk); a ring of the right size with an all-zero header is made anew; a
  store that fails is logged once with its reason
  (`TierManager::store_error()`). The folds of spec §3.4.1 are
  `TierAggregator`, called from `TierManager::store()` while L3 or a custom
  level collects: one L2 record per L2 interval, one L1 record per L1
  interval, flagged `folded`; `query_all()` (`/api/range?level=all`) shows
  each stretch once, at the finest level still on disk.
- **Web:** hand-written HTTP/1.1 + WebSocket (RFC 6455) server in `src/web`;
  the SPA is a string literal in `src/web/spa.cpp`.
- **TUI:** ncurses; polls `/api/samples` once a second.
- **Alerts:** every alert is logged, queued by `LuaEngine::raise_alert()`
  for the serve loop (WebSocket frame `{"alerts":[...]}`, the last 100 for
  `/api/alerts`) and sent to ntfy / Discord / Telegram / SMTP / Twilio
  through `curl(1)` from a worker thread.
- **Rules (on top of spec §3.6):** `cooldown` defaults to `for_ticks`
  ticks; `when()` returning nothing is false (nil skips, as in §3.6.5);
  `action` may be a list (`{ alert, escalate, exec(...) }`); `exec()` in the
  rules file itself returns the command as an action instead of running it
  (`LuaEngine::loading()`).

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
  `WebSocketHub::kMaxClients` (64). On loopback without a password,
  every request's Host must be one of budyk's names (an address,
  localhost, the hostname, `web.allowed_hosts`), or 403: DNS rebinding
  (`own_host()` in `router.cpp`).
- **WebSocket hub:** client fds are non-blocking. `broadcast()` drops a
  client whose send would block or writes a partial frame; `service()`,
  called by the main thread once per tick, answers pings, honours close
  frames and drops clients that hung up; with auth on, `drop_sessions()`
  then closes (1008) every client whose session ended (logout, expiry).
  A vanished client releases L3 on the next tick.
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
tests/smoke/       — daemon-level scripts, one per line in the commands below
addons/            — FreeBSD port + rc.d (files/budyk.in; test-port.sh builds,
                     installs and starts it), systemd unit, Docker; the .deb
                     and .rpm (addons/linux/mkpkg.sh, test-packages.sh)
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
tests/smoke/serve.sh build/src/budyk     # HTTP, WS, SIGHUP, close-on-exec (fds watched 3 s)
tests/smoke/crash.sh build/src/budyk     # SIGKILL + restart, at most one record lost
tests/smoke/warm_grace.sh build/src/budyk  # hot buffer emptied after warm_grace
tests/smoke/escalate.sh build/src/budyk  # a sustained anomaly escalates L2 to L3
tests/smoke/fsck.sh build/src/budyk      # fsck, recovery after a crash, the data_dir lock
tests/smoke/upgrade.sh build/src/budyk OLD/budyk  # rings of an older release converted
tests/smoke/modes.sh build/src/budyk     # metric sets per level, always / on-demand
tests/smoke/auth.sh build/src/budyk      # login, the WebSocket ends with its session,
                                         # a cross-origin WebSocket refused
tests/smoke/resources.sh build/src/budyk # spec §5 items 2-6: RSS, CPU, disk, L1 -> L3,
                                         # catch-up, binary size (release builds; ~2 min)
tests/smoke/crosscheck.sh build/src/budyk  # readings against free, swapinfo, uptime, ps,
                                         # ss / netstat, df, /sys or ifconfig, kern.disks,
                                         # a busy loop and a 64 MB write
tests/smoke/swap_devices.sh build/src/budyk  # FreeBSD, root or sudo: 17 swap devices
                                         # against swapinfo
tests/smoke/net_totals.sh build/src/budyk    # root or sudo: a ping flood over a veth into
                                         # a netns (FreeBSD: an epair renamed as jib names
                                         # it, into a VNET jail) left out of the totals
tests/smoke/cli.sh build/src/budyk       # suggest-rules on the L1 / L2 rings, its write
                                         # errors and --window; the TUI against a silent
                                         # daemon, behind an idle client, a slow reply
tests/check_whitespace.sh                # trailing blanks, tabs, CR, final newline
cmake --build build --target docs        # API reference (Doxygen), any warning fails it
cmake -B build-asan -DSTATIC_LINK=OFF -DENABLE_SANITIZERS=ON   # as CI: ASan + UBSan
cmake -B build-tsan -DSTATIC_LINK=OFF -DENABLE_TSAN=ON         # as CI: ThreadSanitizer
cmake/toolchains/freebsd-sysroot.sh 14.4 amd64 SYSROOT   # then, on Linux:
cmake -B build-fbsd --toolchain cmake/toolchains/freebsd-amd64.cmake -DFREEBSD_SYSROOT=SYSROOT
```

CI is `linux-build.yml` (Linux builds, the .deb and .rpm, ASan + UBSan and
TSan with the smoke scripts, cppcheck, clang-tidy, Doxygen, whitespace,
Docker, coverage, the FreeBSD cross builds and an emulated arm64 FreeBSD)
and `freebsd-build.yml` (14.4 and 15.1: the port and the static
configuration, the smoke scripts, the port installed and started); each
file's header says what it runs. Everything must be green before a merge.
The gates a local build does not show:

- `-DENABLE_WERROR=ON`: `-Wall -Wextra -Wshadow -Wconversion
  -Wsign-conversion` on every target, plus `-Wcast-align=strict` with GCC.
  A new implicit sign or width change, or a cast to a more strictly aligned
  pointer, fails the build.
- Line coverage of `core/` and `storage/` at least 85%
  (`tests/coverage_report.py`).
- The static builds held to the spec §5 figures (`tests/smoke/resources.sh`).

## Coding conventions

- SPDX license header in every file
- In a header, the comment that documents a declaration is Doxygen's:
  `///` above it, `///<` after a member on its line (`/** */`, `/**< */` in
  the C header `sample_c.h`), a file's own description under `/// @file`.
  Comments in `.c`/`.cpp` files and inside function bodies stay `//`. Put
  `<placeholders>`, `\n` and the like in backticks: Doxygen otherwise reads
  them as HTML tags and commands, and its warnings fail CI.
- No exceptions, no RTTI, collectors plain C (spec §3.3 items 1, 7). Lua
  raises errors with `luaL_error` (a longjmp): never hold a C++ object with
  a destructor across a call that may raise — see the pattern in
  `src/rules/lua_stdlib.cpp`.
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
  or none (`apply_seconds`: 300, 300s, 5m, 1h, 1d), booleans any case
  (`True`), a list one string too, and a value that does not read is
  reported under its full name with the default kept. A new key goes in
  `kRootKeys` and its sections in `config.cpp` as well: one it does not
  list is reported as unknown (a typo used to be ignored without a
  word); `test_config` loads `config.example.yaml` and expects silence.
- Every descriptor is close-on-exec, short-lived ones too: `fopen(path,
  "re")` / `"we"`, `O_CLOEXEC`, `SOCK_CLOEXEC`, `mkostemp(..., O_CLOEXEC)`
  (worker threads fork while the main thread has a file open).
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

## Key design decisions (where the code goes past the spec)

1. Lua sandbox (spec §3.6.3) as built: the sample tables are read-only
   proxies, created once and updated in place
   (`src/rules/lua_bindings.cpp`), bound in a protected call with raw sets
   (a failed bind skips the tick's rules; nothing after a failed call may
   allocate on the Lua heap outside one). Instead of a time limit, every
   call into Lua is limited to `rules.limits.instructions` (default
   1 000 000) and the engine to `rules.limits.memory_mb` (default 16).
   Library work in C is charged to the instruction limit too
   (`LuaEngine::charge()`): `src/rules/lua_pattern.c` is Lua 5.4.7's
   own pattern matcher (MIT, kept as Lua writes it, the changes marked
   "budyk") with a step count, `table.move` is charged, past the limit
   the hook raises on every instruction (no `pcall` carries on),
   `xpcall` runs its handler after the call and `setmetatable` refuses
   `__gc` (Lua runs both with hooks off). `exec()` is gated by
   `--enable-exec` or `rules.exec.enabled` plus an allowlist. A runaway
   rule gets an error, not the daemon.
2. The hot buffer (spec §3.4.2) is emptied once no client has been
   connected for `hot_buffer.warm_grace` (`WarmGrace`).
3. Storage records (spec §3.4.3, §3.5) also carry a flags byte (bit 0:
   folded); `write_idx` is advanced only after the `pwrite`. A codec
   version only appends fields; a ring of smaller (older) records is
   converted on start (`convert_ring()`, tests/smoke/upgrade.sh against
   v0.6.4 and v0.7.0; codec v9, 542-byte records, since ICMP). On start
   `TierManager::init()` also counts in a record that reached its slot but
   not the index (`count_unindexed()`), so a daemon crash loses no stored
   record (`tests/smoke/crash.sh`, `fsck.sh`). `budyk serve` holds an
   flock on `<data_dir>/budyk.lock`: one daemon per data_dir. A fold
   window ends after its interval, when the level drops to the ring's
   own, or on shutdown — a SIGKILL loses the open windows, not the raw
   samples.
4. `exec()` is asynchronous: it returns `{ queued = true }` and the outcome
   is logged. `exec(cmd, { timeout = s, wait = true })` is the inline form,
   capped at 60 s.
5. Failed logins are throttled per client address (429 + `Retry-After`);
   no password hash is computed for a throttled attempt.
6. The version string comes from `project(VERSION)` only (`BUDYK_VERSION`).

## Release procedure

1. Bump the version in `project(budyk VERSION x.y.z)` in `CMakeLists.txt`,
   the `.TH` line of `docs/budyk.8` (date too) and `DISTVERSION` in
   `addons/freebsd/Makefile`.
2. In `CHANGELOG.md`, `[Unreleased]` becomes `[x.y.z] — YYYY-MM-DD` with a
   summary paragraph, the tag link goes at the bottom, and a new empty
   `[Unreleased]` opens. That section is the release notes.
3. Open the PR, start a `release.yml` dry run (Run workflow from the branch,
   no `tag`), and merge when it and every check are green.
4. The maintainer publishes on GitHub: **Releases → Draft a new release**,
   tag `vx.y.z` created on publish, target `main` at the merged release
   commit, body empty, **Publish release** (a saved draft starts nothing).
   The other ways in, what gets built, and what to do when the publish job
   gets a 403 are in the header of `release.yml`. The files appear about
   7 minutes later.
5. Verify the published release: tag → commit, the body, 20 assets,
   checksums, `file` on every binary, `budyk version`, and the smoke scripts
   against the published Linux binary.

## Known gaps against the spec

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
