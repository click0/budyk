# Changelog

All notable changes to budyk will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **SIGHUP — rules reload without a restart**. Edit `rules.path`
  (whether `.lua` or `.yaml`), `kill -HUP $(pidof budyk)`, the new
  rule set is live within a tick. Per-rule `cooldown_remaining`,
  `consecutive_hits` and `fire_count` are preserved across the swap
  by name (via a transient state file), so rules in mid-cooldown stay
  quiet through the reload. HTTP server, scheduler, alert channels,
  storage rings, and persistent web sessions are untouched. The
  signal is latched in a `sig_atomic_t` flag and processed at the
  top of the tick loop so an in-flight `eval_tick` can't race
  `engine.shutdown()`. Documented in `budyk(8)`. Alert channels are
  registered once at startup and deliberately *not* re-registered on
  reload — the `AlertDispatcher` is a `LuaEngine` member that survives
  the `shutdown()`/`init()` swap, so re-adding would duplicate every
  channel and fire each alert N+1 times after N reloads. Regression
  guard added in `test_lua_engine` (case 24). A failed state snapshot
  during reload now warns instead of silently zeroing cooldowns.

- **Simple-YAML rule format** — the long-deferred spec §3.6.2
  YAML-to-Lua transpiler is finally real. A `.yaml` / `.yml` rules
  file is detected by extension at load time and converted to regular
  `watch()` calls; the engine sees no difference. Per-rule keys:
  `name*` / `when*` (Lua expression spliced verbatim) + optional
  `for_ticks`, `cooldown`, `severity` (info/warning/critical),
  `action` (alert/log), `message`. Top-level may be a bare sequence
  or a mapping with a `rules:` wrapper. The `when` text is guarded
  against newlines (would let the splice close `function() return …`
  early); bad severities silently fall back to `warning`; malformed
  YAML is rejected with no partial rule registered. Multi-line or
  computed thresholds still want native Lua — the engine accepts
  both transparently in the same `rules.path`.

- **SPA history chart** — the dashboard gains a "History" panel that
  consumes `GET /api/range` and draws a zoomable inline-SVG line chart
  (no JS libraries, still a single-file bundle). Metric buttons
  (CPU % / Mem % / Load 1m / Disk B/s / Net B/s) and range presets
  (1h / 6h / 24h / 7d). Short ranges query the raw L3 ring, longer ones
  the 5-min L1 ring, with an automatic fall-back to the other tier when
  the first is empty (covers the common "daemon mostly at L1" case).
  The window auto-refreshes every 60s. `since` is computed with BigInt
  so the nanosecond bound is exact despite JS float limits.
- **Persistent web sessions** — `SessionStore` now optionally flushes
  its token table to `<data_dir>/sessions.tsv` (atomic temp+rename,
  mode 0600) after every mutation, and reloads it on startup. A daemon
  restart therefore keeps logged-in admins logged in instead of
  bouncing every browser to the login screen — symmetric with the
  rule-cooldown persistence. Expired tokens are dropped on load. The
  file holds live bearer tokens, hence 0600 + a note to lock down
  `data_dir`. Toggle with `web.auth.persist_sessions` (default on).
- **`GET /api/range` — historical query over the on-disk tier rings.**
  The SPA / any client can now read hours-to-months of history, not
  just the 300-record RAM hot buffer. Params:
  `since`/`until` (timestamp_nanos bounds, `until=0` = now),
  `tier` (1 raw L3 / 2 1-min L2 / 3 5-min L1, default 1),
  `limit` (newest-kept, default + hard cap 5000). Backed by a new
  `TierManager::query()` that walks the chosen ring newest→oldest,
  stops at the `since` floor, and returns oldest-first. Reads go
  through `pread` + the atomic `write_idx`, so it's safe to call from
  the HTTP thread while the collector writes; a torn read racing the
  writer fails CRC and is silently skipped. `RingFile` gains a
  `capacity()` accessor.
- **FreeBSD: disk + proc.running + self.rss via kvm/devstat/kinfo_proc**
  — closes three long-standing FreeBSD platform gaps in one PR:
  * `collector/freebsd/disk.c` — previously a `-ENOSYS` stub. Now
    aggregates read/write bytes-per-second across whole disks via
    `devstat_getdevs(3)`, filtered to `DEVSTAT_TYPE_DIRECT` (no
    pass-through). `budyk_collector` links `-ldevstat` (base).
  * `proc.running` — was hard-coded 0. Now counts `kinfo_proc`
    entries with `ki_stat == SRUN` in the same `kern.proc.all`
    walk that yields `total`, so the second metric is essentially
    free.
  * `self.rss_bytes` — was 0 (we relied on `peak_rss_bytes` from
    getrusage). Now reads `kinfo_proc.ki_rssize × getpagesize()`
    via `sysctl(KERN_PROC_PID, getpid())`.

- **File-watch sparkline** in the SPA — the new "Files" row now also
  carries a unicode sparkline (`▁▂▃▅▇█`) over the last 30 ticks
  (~2.5 h at default L1 cadence), so an operator can see *when*
  changes happened, not just the current tick. Backfilled from the
  `/api/samples` catch-up on page load so a refresh doesn't lose
  the visible timeline.
- **File-watch history on the dashboard** — codec **v7** appends a
  `file_watch` block (`events_this_tick`, `watched_count`, `present`)
  to the `Sample`, so file-change activity is persisted to the storage
  rings, served via `/api/samples`, and rendered as a new SPA row
  (hidden unless `security.file_watch` is enabled). `events_this_tick`
  is the count of distinct watched paths that fired an event since the
  previous tick — a spike on the timeline marks exactly when tampering
  happened. Decoder still reads v1–v6 records (file_watch zero-filled);
  record size grows 240 → 256 B (storage record 270 B).

- **Persistent rule cooldown state** — `LuaEngine::save_state` /
  `load_state` serialize per-rule `cooldown_remaining`,
  `consecutive_hits` and `fire_count` to `<data_dir>/rule_state.tsv`
  (overridable via `rules.state_path`). `cmd_serve` restores on
  startup after the rules load, flushes every 60s + on graceful
  shutdown. A restart mid-cooldown therefore no longer re-fires the
  rule — prevents an alert-storm on daemon bounce. Cooldown persists
  as a tick count, so a long downtime doesn't decrement it (errs
  toward staying quiet). Toggle with `rules.persist_state` (default
  on). Writes are atomic (temp + rename).

- **File change watcher** (`src/security/file_watcher`) — cross-platform
  surface over `inotify` (Linux) and `kqueue + EVFILT_VNODE` (FreeBSD)
  for tamper-detection rules ("alert when /etc/sudoers changes"). API
  is `init` / `add(path)` / `poll(timeout_ms, &events)` / `shutdown`;
  events carry `path` + `kind` (Modified / Deleted / Created). Per-poll
  coalescing collapses a flurry of editor `write(2)`s into one
  Modified event per path. CLI: `budyk watch-files [--timeout MS]
  <path>...` for ad-hoc diagnostic. `cmd_serve` wires it into the tick
  loop when `security.file_watch.{enabled,paths}` is set, exposing
  events as the `files` Lua global with per-path `.modifies`,
  `.deletes`, and a tick-scoped `.tampered` flag (`FileWatchState::apply`
  clears the tampered set before each batch, so `when = files[p].tampered`
  fires exactly once per detected event).
- **`freeze()` / `unfreeze()` Lua actions** — incident-response surface
  for the rule engine: `freeze(pid)` sends SIGSTOP, `unfreeze(pid)`
  sends SIGCONT. Both raise an error unless the engine was started
  with `--enable-freeze` (or `rules.freeze.enabled: true`), and both
  honour `rules.freeze.allow: [...]` — a whitelist of process names
  (kernel `comm`) the bindings are permitted to signal. `proc_name_of`
  resolves the target via `/proc/<pid>/comm` on Linux and
  `kinfo_proc.ki_comm` on FreeBSD.
- **Multi-channel alert dispatcher** — `AlertChannel.type` now routes
  to **ntfy.sh / Discord / Telegram / SMTP / Twilio**. ntfy and
  Discord shipped in PR #52; Telegram, SMTP and Twilio land here.
  Each backend reuses the existing `popen(curl …)` plumbing and
  writes credentials through `curl --netrc-file` so SMTP usernames
  / Twilio Account SIDs never appear in `ps`.
- **Config schema** — `alerts.channels: [...]` block in `config.yaml`
  with per-entry `{name, type, url, topic, token, from}`. Channels
  with no `type` are silently dropped (no-op); `cmd_serve` walks the
  list and calls `engine.alerts().add_channel(...)` for each, logging
  the registered count to stderr. Documented in `config.example.yaml`.
- **Payload builders** exported for tests: `telegram_payload`,
  `smtp_message` (full RFC 5322 blob with `Date:` / `MIME-Version:` /
  `Content-Type:`), `twilio_form` (URL-encoded `From/To/Body`).

## [0.4.0] — 2026-05-07

Closes the entire spec §3.3.3 metric set — the `Sample` struct now
covers every block the design ever called out: cpu / mem / swap /
load / disk / net / proc / entropy / self / thermal.

### Added

- **`ProcessStats`** — `proc.{total, running}` from `/proc/loadavg`
  field 4 on Linux, `kern.proc.all` size-only sysctl on FreeBSD.
  `running` stays at 0 on FreeBSD until a `kinfo_proc.ki_stat`
  walk lands; `total` is exact on both. (PR #46)
- **`EntropyStats`** — `entropy.{available_bits, present}` from
  `/proc/sys/kernel/random/entropy_avail` on Linux. FreeBSD has no
  semantically equivalent surface — `present == false` there. The
  SPA hides the row when `present == false`. (PR #47)
- **`SelfStats`** — `self_.{rss_bytes, peak_rss_bytes,
  cpu_user_seconds, cpu_system_seconds}`. Linux pulls current RSS
  from `/proc/self/statm`; both platforms use `getrusage(2)` for
  peak RSS and CPU time. Lets users see the daemon's own
  footprint via `/api/samples` or rule-engine alerts. (PR #48)
- **`ThermalStats`** — `thermal.{max_celsius, sensor_count,
  present}` — hottest sensor reading across `/sys/class/thermal/
  thermal_zone*/temp` on Linux and the `dev.cpu.<N>.temperature`
  sysctl chain on FreeBSD. Soft-fails to `present == false` on
  hosts without ACPI / IPMI passthrough. (PR #49)

### Changed

- **Codec versioned to v6** in four steps:
  - v3 (PR #46) — adds `proc` (8 B). 184 B per sample.
  - v4 (PR #47) — adds `entropy` (8 B). 192 B.
  - v5 (PR #48) — adds `self` (32 B). 224 B.
  - v6 (PR #49) — adds `thermal` (16 B). 240 B.
  Storage record is now `14 header + 240 = 254` bytes. `sample_decode`
  honours every prior version (v1..v5) — historical ring-file
  records remain readable, the missing tail fields read as zero.
- **Lua bindings** gained `proc`, `entropy`, `self_`, and `thermal`
  globals — rule `when()` bodies can now reference any metric in
  the spec.
- **JSON `/api/samples`** + **WebSocket** push gained the four
  matching blocks. The single-page UI (`src/web/spa.cpp`) added
  three new dashboard rows: "Processes", "Entropy" (Linux-only,
  hidden on FreeBSD), "Thermal" (hidden when no sensors), and
  "budyk RSS" for the daemon's own footprint.
- **CI** bumped `cross-platform-actions/action` from v0.32.0 to
  v1.0.0 (latest GA, April 2026). FreeBSD jobs continue to flake
  on the upstream Vagrant Cloud SSH bootstrap (`exit 8`) — that's
  an infra-side issue independent of our code; the bump just keeps
  us on a maintained release. (PR #50)

### Tests

19 ctest suites still cover every code path. The codec, JSON,
Lua-engine, and Linux/FreeBSD collector tests grew assertions
for each new metric block; new backward-compatibility cases
encode-then-patch the version byte to verify v3, v4, v5 records
decode cleanly with the missing tail fields zeroed.

[0.4.0]: https://github.com/click0/budyk/releases/tag/v0.4.0

## [0.3.1] — 2026-04-30

Patch release — paper cuts surfaced while smoke-testing the v0.3.0
release binary end-to-end.

### Fixed

- **`budyk serve`** no longer prints a misleading
  *"rules file '/usr/local/etc/budyk/rules.lua' failed to load"*
  warning on every fresh-install start. The path is now
  `::access(R_OK)`-probed first; the warning fires only when a rules
  file is present **and** `LuaEngine::load_file` fails to parse it.

### Changed

- `config.example.yaml`:
  - Real GitHub URL instead of the `USER/budyk` placeholder.
  - Added the nested `rules.exec.{enabled,allow}` block (PR #24)
    with two realistic allowlist entries.
  - Explicit comment on `web.auth.password_hash`: wrap it in quotes
    because the PHC string contains `$` / `,` / `=`, which YAML's
    flow style treats as separators / map keys.
  - Section dividers so the file scans cleanly when copied into
    `/etc`.
- `docs/budyk.8` dated 2026-04-30; `main.cpp` `version` command and
  `/api/health` JSON both report `0.3.1`.

[0.3.1]: https://github.com/click0/budyk/releases/tag/v0.3.1

## [0.3.0] — 2026-04-29

### Added

- **Live daemon: `budyk serve`** — single-thread main loop wires
  `Config` → platform collectors → `Scheduler` → `TierManager` →
  `HotBuffer` → `LuaEngine`. SIGINT/SIGTERM trigger a graceful
  shutdown; SIGPIPE ignored.
- **`TierManager`** — routes encoded samples by `Sample::level` to
  three on-disk ring buffers (250/150/50 MiB defaults).
  `init()` / `store()` / `close()` / `tier{1,2,3}_count()`.
- **CLI completion**:
  - `budyk hash-password` — interactive (TTY echo-off) or piped
    Argon2id hash, ready for `password_hash:` in `config.yaml`.
  - `budyk suggest-rules` — reads from `tier1.ring` and runs
    `ai::suggest_rules_for_samples()`. Window arg
    (`<N>{s,m,h,d}`), `--config`, `--output`, and `--ai`.
  - `budyk tui` — ncurses dashboard polling `/api/samples` once a
    second. Gauge bars for CPU / Memory / Swap / Load, text panels
    for Disk / Net / Uptime. `q` / ESC to exit.
- **Embedded HTTP/1.1 server** (`src/web/server.{h,cpp}`):
  accept-loop on a worker thread, full header + body parsing
  (`Content-Length` capped at 64 KiB → 413), `extra_headers` on the
  response (Set-Cookie / Cache-Control), `hijack` callback for
  long-lived connections.
- **Endpoints**:
  - `GET  /api/health` — public liveness JSON.
  - `GET  /api/samples` — JSON dump of the hot-buffer.
  - `POST /api/auth/login` — Argon2id verify against
    `cfg.password_hash`, sets `Set-Cookie: budyk_session=...; HttpOnly;
    SameSite=Strict`.
  - `POST /api/auth/logout` — revokes the cookie.
  - `GET  /api/ws` — RFC 6455 WebSocket upgrade. Handshake produces
    `Sec-WebSocket-Accept = base64(SHA1(key + magic))` from a
    pure-C in-tree implementation. Catch-up frame on connect; one
    text frame per collector tick.
- **`SessionStore`** — in-process token table, 24-h TTL default,
  lazy-evicting on `verify()`. Tokens are 32-byte hex from
  `web::auth::new_session_token()`.
- **AI Tier B** — `budyk suggest-rules --ai` calls Anthropic's
  `/v1/messages` via `popen("curl …")`. API key passed in a temp-file
  header bundle so it never lands on `ps`. Default model
  `claude-haiku-4-5-20251001`.
- **Single-page web UI** — self-contained HTML/CSS/JS in
  `src/web/spa.cpp` (one raw-string literal). `GET /` serves it
  verbatim. JS probes `/api/samples`, falls into a login form on 401,
  then opens `/api/ws` and live-updates with a 2 s reconnect backoff.
- **FreeBSD collector suite** — five real implementations replacing
  the ENOSYS stubs:
  - `freebsd/cpu.c`        — `kern.cp_time` deltas + `hw.ncpu`.
  - `freebsd/memory.c`     — `vm.stats.vm.*` for RAM and
    `kvm_getswapinfo` for swap (soft-fail in jails).
  - `freebsd/system.c`     — `kern.boottime` for uptime, `getloadavg(3)`.
  - `freebsd/network.c`    — `getifaddrs(3)` + `AF_LINK` byte
    counters, loopback excluded.
  - `freebsd/disk.c`       — written but not yet enabled (FreeBSD CI
    infra regression in cross-platform-actions, tracked separately).
- **CMake `BUDYK_LINUX` / `BUDYK_FREEBSD` macros** wired through
  `target_compile_definitions(budyk_core PUBLIC ...)` so source files
  can `#ifdef`-dispatch on the configured platform.
- **`tier_manager` + `storage_codec` direct test coverage** (the
  CRC32C path was previously only exercised indirectly through
  `test_ring_file`).

### Changed

- `docs/budyk.8` dated 2026-04-29; `main.cpp` `version` command and
  `/api/health` JSON both report `0.3.0`.
- `tests/CMakeLists.txt` grew six new suites:
  `test_tier_manager`, `test_storage_codec`, `test_freebsd_collector`
  (gated), `test_session`, `test_ws`, `test_http_server`,
  `test_json`, `test_llm_client`. Total ctest count: **19**.

[0.3.0]: https://github.com/click0/budyk/releases/tag/v0.3.0

## [0.2.0] — 2026-04-22

### Added

- **Linux disk throughput** via `/proc/diskstats`: aggregated
  read/write bytes per second across whole block devices only.
  Filters out `loop*`, `ram*`, `zram*`, `dm-*`, `md*`, `fd*`, `sr*`,
  `nbd*` and partitions (`sdX<N>`, `nvme<N>n<M>p<K>`, `mmcblk<N>p<K>`,
  …).
- **Linux network throughput** via `/proc/net/dev`: aggregated
  rx/tx bytes per second across non-loopback interfaces.
- **Sample codec v2** — 176-byte record layout now serialises the
  disk + net aggregates. v1 records (128 B) remain decodable; the
  codec falls back to zeroed disk/net fields for them.
- **Lua bindings for `disk` and `net`** — rule `when()` bodies can
  reference `disk.read_bytes_per_sec`, `disk.write_bytes_per_sec`,
  `disk.device_count`, `net.rx_bytes_per_sec`, `net.tx_bytes_per_sec`,
  `net.interface_count`.
- **AI Tier A rule suggestions** for the four new throughput metrics
  — `disk_read_high`, `disk_write_high`, `net_rx_high`, `net_tx_high`
  — with idle-metric skip, p99-scaled threshold, per-metric MiB/s
  floor, and B/KiB/MiB/GiB pretty-printing in rationale comments.
- **`exec()` rule action** — `fork`/`execvp` helper with a hard
  `timeout_seconds` deadline, `SIGKILL` on overrun, `RLIMIT_CPU` and
  `RLIMIT_AS` caps, and stdio redirected to `/dev/null`. Wired into
  the Lua stdlib as both `exec("/path")` and
  `exec({"/bin/sh", "-c", "..."})`; returns an `{exit_status, signal,
  timed_out, elapsed_seconds, ok, error?}` result table.
- **`exec()` hardening** — three layers of defence against adversarial
  rules: argv[0] must be an absolute path, no `..` path-segment
  traversal, and an optional `LuaEngine::set_exec_allowlist()` that
  restricts argv[0] to an exact match against the configured list.
- **YAML `rules.exec.{enabled,allow}`** block — admins can declare
  the allowlist in `config.yaml`. Legacy `rules.enable_exec` flat key
  still honoured.

### Changed

- `docs/budyk.8` dated 2026-04-22; `main.cpp` `version` command prints
  `budyk 0.2.0`.

[0.2.0]: https://github.com/click0/budyk/releases/tag/v0.2.0

## [0.1.0] — 2026-04-18

First milestone release.

### Added

- 3-level adaptive collector model (L1 heartbeat / L2 watchful / L3 active)
  with anomaly-triggered escalation and client grace period (spec §3.3).
- Tiered ring-buffer storage: 64-byte mmap'd header, `pwrite` records,
  atomically-updated `write_idx`, CRC32C (Castagnoli) per record
  (spec §3.4).
- Pure-math L3→L2/L1 tier aggregator: mean fold for percentages/load,
  last-value for totals (spec §3.5).
- 300-record in-memory hot buffer for WebSocket catch-up; RAM-only, never
  touches disk (spec §3.5.3).
- Embedded Lua 5.4 rule engine (spec §3.6):
  - Sandbox: only `_G` + `math` + `string` + `table` are opened;
    `dofile`, `loadfile`, `load`, `loadstring`, `require` are stripped.
  - `watch(name, opts)` registry with `when`, `action`, `for_ticks`,
    `cooldown` fields.
  - `for_ticks` sustain counter and `cooldown` skip window.
  - `exec()` gated behind the `--enable-exec` flag (recognised but
    fork/timeout not yet implemented).
- AI Tier A suggestions (spec §6):
  - Local `MetricBaseline` statistics — min/max/mean/stddev/p95/p99
    via nearest-rank percentiles.
  - Lua `watch()` generator with rationale comments for `high_cpu`,
    `memory_low`, `swap_pressure`, `load_high`.
- Argon2id password hashing via `libargon2` (OWASP 2024 defaults:
  t=3, m=64 MiB, p=4) and random 32-byte session tokens sourced from
  `/dev/urandom` (spec §3.7.3).
- Linux collector MVP:
  - `/proc/meminfo` → `mem.total` / `mem.available` / swap.
  - `/proc/stat` CPU delta via `budyk_cpu_ctx_c`.
  - `/proc/uptime` + `getloadavg(3)`.
- YAML configuration loader using `libyaml` DOM walk covering
  `collection.*`, `storage.*`, `rules.*`, `web.auth.*` sections
  (spec §4).
- Packaging:
  - FreeBSD port skeleton (`USE_GITHUB`, `DISTVERSIONPREFIX=v`,
    `LIB_DEPENDS` on `libargon2` and `libyaml`, `USE_RC_SUBR`
    with `daemon(8)` wrapper, `pkg-plist`, `pkg-descr`).
  - Hardened `systemd` unit (`PrivateTmp`, `ProtectKernelTunables`,
    `ProtectSystem=strict`, `MemoryDenyWriteExecute`, ...).
  - Multi-stage Alpine-based Dockerfile with dedicated `budyk` user.
- CI: `ubuntu-latest` Linux build + `cross-platform-actions` FreeBSD
  14.2 / 15.0 matrix with weekly cron; lite workflow runs FreeBSD 14.2
  smoke on non-main branches.

### Known limitations

- No HTTP server yet (M5); no WebSocket hub (M6); no TUI (M8).
- `TierManager` ring-file wiring is not yet connected.
- No `/proc/diskstats` or `/proc/net/dev` deltas; FreeBSD sysctl /
  devstat / kvm collectors are scaffolded but not implemented.
- `exec()` action is recognised but fork / timeout is not implemented.
- Signed-artefact release workflow is deferred.

[0.1.0]: https://github.com/click0/budyk/releases/tag/v0.1.0
