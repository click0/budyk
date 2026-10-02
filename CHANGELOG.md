# Changelog

All notable changes to budyk will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **`sha256sum -c *.sha256` works on a full set of release assets.**
  The FreeBSD sidecars were written by `sha256 -r` (one space between
  hash and name), the Linux ones by `sha256sum` (two spaces). Given
  both in one run, GNU `sha256sum -c` misread the Linux lines and
  reported them as unreadable; each file checked on its own was fine.
  FreeBSD sidecars are now in the GNU format too, and a new
  `checksums` job in the release workflow runs `sha256sum -c` over all
  of them at once, on the dry run as well, before anything is
  published.

## [0.6.2] — 2026-10-01

A small update. `budyk tui` now shows the current collection level,
including custom levels, the way the dashboard does. The dashboard no
longer logs a 404 for `/favicon.ico` on every load. CI now measures
line coverage and keeps `core/` and `storage/` at or above 85%; adding
it turned up two untested storage paths, which are now covered.

### Added

- **The TUI shows the current level,** as the dashboard does: a
  `Level  burst (every 0.5 s)` row. The names come from `/api/levels`.
  The table is fetched again (at most every 30 ticks) if a sample
  carries an unknown id, for example after the daemon is restarted
  with other custom levels.

### Fixed

- **The dashboard no longer requests `/favicon.ico`.** The browser
  asked for it on every load, got a 404, and logged an error in the
  console. The page now declares an empty icon.

### CI

- **Line coverage is measured and gated.** A new coverage job runs the
  unit tests and the serve smoke test (which covers `main.cpp`) under
  gcov/lcov. `tests/coverage_report.py` prints coverage per module,
  writes it to the job summary, and fails the job if `core/` or
  `storage/` drops below 85% (spec §5 item 10). Current figures:
  core 100%, storage 96.9%, scheduler 100%, web 93.8%, total 81%.
- **Tests for paths coverage showed were never run.** `query_all()`'s
  thinning across rings never ran in tests: each ring came back
  already within the limit, so the merge step had nothing to thin. A
  three-ring test now exercises it. So does a custom ring whose size no
  longer matches `storage_mb`: init returns -9, closes what it had
  opened, and init with the original size works again. tier_manager.cpp
  coverage went from 84.3% to 94.4%.

[0.6.2]: https://github.com/click0/budyk/releases/tag/v0.6.2

## [0.6.1] — 2026-09-30

Bug fixes for `budyk tui`, all present since the TUI was added. With a
password-protected dashboard it showed made-up zeros (0% CPU, 0 cores,
free 0B / 0B) instead of logging in; it now asks for the password once
and keeps the session alive. Its values were read from the wrong
fields: the memory total showed the process count. And the memory line
ran off the screen at every width. CI now also builds with
`-Wall -Wextra` and treats warnings as errors.

### Fixed

- **`budyk tui` didn't work with a password-protected dashboard.** It
  called the API without logging in, got 401, and drew made-up zeros
  (0% CPU, 0 cores, "free 0B / 0B") as if they were real readings. It
  now asks for the password once at startup (no echo), logs in, and
  sends the session cookie with every request. When the session
  expires, for example after a daemon restart with
  `persist_sessions: false`, it logs in again on its own. A wrong
  password exits with a clear message. Any other non-200 answer is
  shown on screen instead of zeros.
- **The TUI read the wrong numbers.** Each value came from the last
  occurrence of its key anywhere in the response. The memory total
  therefore came from `proc.total`, the process count, and "cores"
  could come from another section. Values are now looked up within
  their own section of the newest sample.
- **The TUI's memory line ran off the screen.** The bar was sized
  `COLS - 30`, but the memory line needs 40 columns besides the bar, so
  the memory total was cut off at every terminal width. That is also
  why the wrong total above was never visible.

### CI

- **Compiler warnings are on, and fatal in CI.** `-Wall -Wextra` is
  now always enabled. Before, not even `-Wall` was on. A new
  `ENABLE_WERROR` option turns warnings into errors. CI sets it on the
  Linux static/shared, sanitizer and FreeBSD lite/full builds, but not
  the release build, so a warning from a newer compiler can't block a
  release. The only warnings were `-Wmissing-field-initializers` from
  aggregate initialisation, where omitted fields took safe defaults.
  `HttpResponse` gains a (status, type, body) constructor instead of
  trailing `{}`s, and `LuaRule` initialises `last_error` explicitly.
  GCC 13 and Clang 18 build with 0 warnings.

[0.6.1]: https://github.com/click0/budyk/releases/tag/v0.6.1

## [0.6.0] — 2026-09-27

Custom collection levels: your own cadences next to L1–L3, such as
2 Hz under pressure or every 30 min on an idle box. Each one is switched
on by a Lua condition or by `escalate()` from a rule, and has its own
ring file. The active level with the highest priority wins. Also a
security fix: alert channel settings could run shell commands, because
curl ran through `system()`; curl now runs without a shell. CI now runs
ASan/UBSan, a smoke test of the live daemon, cppcheck and clang-tidy on
every change. Those runs found a crash on `hot_buffer.capacity: 0`, a
memory leak in `exec()` and an integer overflow, all fixed here.

### Added

- **Custom collection levels.** `collection.levels` defines up to 16
  levels of your own next to L1–L3, each with its own cadence, for
  example 2 Hz under pressure or every 30 min on an idle box. Each level
  has:
  - `name`, `interval` (seconds, fractions allowed, 0.1–86400) and
    `priority` (required);
  - an optional `when`, a Lua expression over the metric globals, and
    `hold`, seconds to stay active after `when` was last true;
  - an optional `storage_mb` for the level's own ring file
    `level-<name>.ring`.
  The active level with the highest priority wins, ties going to the
  shorter interval. Built-ins: L1 = 0 (always active), L2 = 20,
  L3 = 30. An invalid level is skipped with a logged reason; the rest
  load.
- **`escalate(level, seconds)` works.** It was a no-op. A rule can now
  keep a custom level, `"L2"` or `"L3"` active for `seconds` (default
  60), starting with the current sleep. An unknown level raises.
- **`GET /api/levels`** returns the level table (`id`, `name`,
  `interval_ms`, `priority`, `builtin`), so clients can name the
  `level` id every sample carries.
- **`/api/range?level=`** takes `L1`–`L3`, a custom level's name, or
  `all`. `all` merges every ring. Past the limit it's thinned by time
  across the whole window, not cut to the newest samples. Each ring is
  read with a binary search for the window bounds plus evenly spread
  reads, so the cost doesn't grow with the window.
- **Dashboard:** a "Level" row shows the current level by name and
  interval. The history chart reads `level=all`, so it covers every
  level. Before, it read only the L3 and L1 rings and ignored L2.

### Changed

- **Rule errors are logged.** A `when` or function action that raises
  used to be dropped silently by `pcall`. It's now logged as
  `budyk: rule '<name>' when|action failed: <error>`, once per distinct
  error, so a failing condition doesn't flood the log. A custom level's
  `when` condition is logged the same way.
- **Level ids 4–19 are valid** in stored records and in the sample
  codec; the on-disk format is unchanged. Before, anything but 1–3 was
  rejected.

### Security

- **Alert channel settings could run shell commands.** Alerts ran curl
  through `system()`:
  - ntfy, Discord and Telegram put the channel URL into the command
    unquoted;
  - Twilio did the same;
  - SMTP put `from` / `topic` in single quotes.
  A `;`, `$(...)` or `'` in those config fields therefore ran commands
  as the daemon user. Confirmed live: an ntfy URL of
  `http://…/;touch FILE;` created FILE. curl is now started directly
  with `fork` + `execvp` (via `exec_command`), each value one argv
  entry, and every URL passed with `--url`, so one starting with `-`
  can't become an option. The fields are config-only, so an attacker
  needed write access to the config, but a stray quote in an e-mail
  address or token also broke delivery. Checked live that ntfy (with
  its Title / Priority headers), Twilio (netrc basic auth with a `'`
  in the token) and SMTP (a fake server received `o'brien@…`
  verbatim) still deliver. `test_alert` case 14 fails against the old
  code.

### Fixed

- **`collection.hot_buffer.capacity: 0` crashed the daemon** with
  SIGFPE on the first tick (a modulo by the capacity). An interval of
  0 or less for L1–L3 would have made the collection loop spin a core.
  Out-of-range numbers in the config now fall back to their defaults
  and are logged:
  - port;
  - the L1–L3 intervals, hysteresis and grace period;
  - the hot buffer's capacity and warm grace;
  - the `tier*_max_mb` sizes.
  HotBuffer also treats capacity 0 as 1 and can no longer be copied (a
  copy would double-free its array).
- **`exec()` leaked memory on every rejected call.** It built its argv
  in C++ containers, then raised with `luaL_error`, which longjmps past
  their destructors. This hit a bad argv, a relative path, `..` and
  allowlist rejections. The error is now raised after cleanup. Found
  by the new ASan job.
- **`exec(cmd, timeout)` with a huge timeout** wrapped through `int`,
  and `timeout + 5` could overflow (undefined behaviour). The timeout
  is now capped at 86400 s, and the CPU rlimit is computed in `rlim_t`.
- **`/api/range` number parameters** wrapped around on values past
  2^64; they now saturate.
- **`HttpResponse::status` defaulted to an uninitialised value.** The
  WebSocket upgrade path never set it. It now defaults to 200.

### CI

- **ASan + UBSan job.** It runs every unit test plus
  `tests/smoke/serve.sh`, a smoke test of the running daemon: every
  endpoint, a WebSocket client, a dropped connection, garbage input,
  SIGHUP and a clean SIGTERM. Any sanitizer report fails the job. The
  new `ENABLE_SANITIZERS` CMake option makes such a build locally; it
  needs `STATIC_LINK=OFF`.
- **cppcheck + clang-tidy job.** clang-tidy uses bug-finding checks
  only (`bugprone-*`, `clang-analyzer-*`, minus the noisy ones, listed
  with reasons in `.clang-tidy`). Any finding fails the job.
- **Test asserts stay active in Release builds.** Most tests call the
  code under test inside `assert()`. With `NDEBUG` those calls were
  compiled away and the tests passed without running anything; the
  test targets now build with `-UNDEBUG`.

[0.6.0]: https://github.com/click0/budyk/releases/tag/v0.6.0

## [0.5.0] — 2026-09-26

First release shipped as fully static binaries for Linux and FreeBSD
14.2 / 15.0, a single file with no runtime library dependencies.
Operationally: rules reload on SIGHUP and can be written in a simple
YAML form, alerts go out to external channels (ntfy, Discord,
Telegram, SMTP, Twilio), and a file-change watcher plus
`freeze()` / `unfreeze()` rule actions add an incident-response
surface. On the dashboard: a history chart over
the on-disk tiers (`GET /api/range`), and web sessions that survive
a restart. On FreeBSD, the disk, running-process and self-RSS
metrics are now collected; they were placeholders before.

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

### Fixed

- **Default build (`STATIC_LINK=ON`) failed to link on Linux.** `-static`
  was passed, but every dependency (Lua, Argon2, libyaml, ncurses)
  resolved to its shared object, so the link died with "attempted static
  link of dynamic object". With `STATIC_LINK=ON`, library lookup is now
  restricted to static archives; `-static` applies to every executable,
  tests included (glibc's `libm.a`, pulled in via Lua, only links into a
  static binary); and static ncurses gets `libtinfo` appended. Result: a
  fully static `budyk`, ~3.0 MB stripped on amd64. `STATIC_LINK=OFF` is
  unchanged.
- **Opening the dashboard never switched to L3.**
  `Scheduler::client_connected()` existed but nothing called it, so
  the client count stayed 0. The level also only changed after the
  collection loop woke from `nanosleep`, which at L1 takes up to
  5 min. A connected dashboard got one sample per L1/L2 interval
  instead of 1 Hz, confirmed with a live WebSocket client. Now:
  - before every tick the loop sets the client count to the open
    WebSockets plus one for a `/api/samples` poll in the last 5 s
    (the TUI), through the new `Scheduler::set_client_count()`;
  - the loop sleeps in `poll()` on a self-pipe, and a new WebSocket
    client or a newly appearing poller wakes it at once.
  The first live L3 sample now reaches a new dashboard at once, then
  1 Hz; spec §5 asks for ≤ 2 s. After the last client leaves, the
  grace period applies and the level steps back to L1.
- **SIGHUP is applied immediately.** It used to wait for the current
  sleep to end, up to 5 min at L1. It now wakes the loop, like
  SIGTERM / SIGINT.
- **Rules without a function action sent nothing.** `watch()` stored
  `action = "alert"` / `"log"` and the default (no `action`) as a tag,
  but nothing acted on the tag. `action = alert` (the form in the
  shipped examples and the old README) called `alert()` with no
  arguments, which raised inside `pcall`. `severity` was never read.
  In all these cases the rule fired and was counted, and no
  notification went out. Now:
  - `"alert"` and the default send `message` (default: the rule name)
    with `severity` (default: warning) to every channel;
  - `"log"` writes `[budyk] <message>`;
  - `action = alert` means `"alert"`.
  An action table, an unknown action or severity, or a non-string
  message is now a load error instead of a silent no-op.
  `rules/examples.lua` and `rules/freebsd-defaults.lua`, which used
  `action = { alert, escalate }`, are rewritten and load cleanly.
- **Rule load errors are logged with their cause.** The daemon used to
  print only `failed to load (rc=-2)`. It now prints the Lua error
  (file, line, rule name) and how many rules above the error are still
  active. Before, it said "continuing without rules" even when some
  rules had loaded.
- **`cooldown` default documented correctly.** It is 0. The comment in
  `watch()` said it defaulted to `for_ticks`.
- **Missing Lua 5.4 now fails at configure time** with an install hint,
  instead of printing "will use vendored copy from third_party/" (no such
  copy exists) and failing later on a missing `lauxlib.h`.

### CI

- Linux workflow builds both link modes. The `static` leg configures
  with no `-DSTATIC_LINK` flag, so it tests the actual default, and
  asserts the binary is statically linked. Previously every job passed
  `-DSTATIC_LINK=OFF`, so the default was never built.
- **Release binaries are now fully static** on Linux and FreeBSD
  (`-DSTATIC_LINK=ON` in all four release builds, previously `OFF`).
  Each build fails unless `file` reports "statically linked", so a
  dynamic binary can't be published.
- **Release notes are the version's CHANGELOG section**, not the whole
  file. A tag whose version has no `## [X.Y.Z]` section fails the
  release instead of publishing without notes.

### Documentation

- **README rewritten, plus a Ukrainian version (`README.uk.md`).** It
  now covers installation (release binaries, build dependencies,
  services), configuration, the rule API (`watch()` options, built-in
  functions, metric globals, the YAML form), alert channels, the HTTP
  API, signals and supported platforms. Both files ship in the release
  tarballs. The old README's rule example used `action = alert` and a
  `severity` field. That form sends nothing: the action is called with
  no arguments, and `watch()` ignores `severity`. The example now uses
  a working form. See Fixed below for the engine side. A "How it works"
  section (both languages) shows the data path: one daemon per server
  serves its own dashboard, with no central collector. It also shows
  when each level applies, how the dashboard and TUI get samples, and
  that each level has its own ring file (tier 1 = L3 samples, 2 = L2,
  3 = L1; these are not aggregates).

[0.5.0]: https://github.com/click0/budyk/releases/tag/v0.5.0

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
