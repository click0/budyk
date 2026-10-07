# Changelog

All notable changes to budyk will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Security

- **Stricter HTTP request parsing.** The parser accepted a
  `Content-Length` with trailing garbage (`12abc` read as 12), took the
  first of two disagreeing `Content-Length` headers (a request-smuggling
  shape behind a proxy), ignored `Transfer-Encoding` and read no body,
  and closed a connection whose headers outgrew 16 KiB without a word.
  Now every `Content-Length` must be all digits and duplicates must
  agree, or the answer is 400; any `Transfer-Encoding` gets 501; an
  oversized header block gets 431. The request line must be
  `METHOD /target HTTP/1.x`, and header lines need a name without
  whitespace (no obsolete folding), or the answer is 400.
- **Ring files are created 0600.** They were 0644, so any local user
  could read the history (load, memory, process counts, file-watch
  events). Existing files keep their mode.
- **`suggest-rules --ai` runs curl without a shell.** The curl command
  line was put together as a string from the temp-file paths and run
  with `popen(3)`. Those paths come from `$TMPDIR`, so a `TMPDIR` with
  a space broke the call and one with `;`, `$(...)` or backquotes ran
  commands. curl is now started directly (fork + exec, as the alert
  channels do), the response and curl's own error message go to
  private temp files, and a failure says what went wrong ("curl(1)
  exited with status 6: Could not resolve host ...", or the API's error
  body) instead of `rc=-4`. The unit test runs it against a stand-in
  `curl` with such a `TMPDIR`.

- **Sample tables are read-only to rules.** `cpu`, `mem`, `swap`,
  `load`, `disk`, `net`, `proc`, `entropy`, `self_` and `thermal` were
  ordinary Lua tables, so a rule that wrote `cpu.total_percent = ...`
  changed what every later rule saw on that tick, although the header
  and spec §3.6 said the values were read-only. They are now proxies:
  an assignment raises "cpu.total_percent is read-only" at the rule's
  line (logged once, like any rule error), `pairs()` still iterates the
  fields, `getmetatable()` returns "read-only" and `setmetatable()`
  cannot replace it. A rule that reassigns the global or `rawset()`s a
  field affects that tick at most.
- **The session cookie is `Secure` behind a TLS proxy.** When the
  login request carries `X-Forwarded-Proto: https`, the cookie (and the
  logout cookie) get `Secure`, so the browser never sends the session
  token over plain HTTP. Plain-HTTP requests keep the old attributes,
  since a `Secure` cookie would not be sent back at all.

- **The `exec()` allowlist is compared by resolved path.** Entries
  were matched by spelling, so `/usr/bin//x` or `./` in a path slipped
  past an exact entry, and an allowed path replaced by a symlink to
  another program still matched. The list is now resolved with
  `realpath(3)` when it is set, and the command's resolved path is
  compared against it: a symlink to an allowed binary and a path with
  `//` are allowed; an allowed path later re-pointed at another binary
  is refused, because it resolved to the original when it was allowed.
- **Start-up warnings for two risky set-ups.** A config or rules file
  that group or others can write (whoever can write `rules.lua` runs
  Lua as the daemon) gets a warning with its mode; `web.auth.enabled`
  with a listen address off loopback gets one too, since budyk speaks
  plain HTTP and the password and session cookie would travel in the
  clear. The serve smoke test checks the first.

### Fixed

- **`/api/samples?x=1` is no longer a 404.** Only `/api/range` split
  the query string off the request target; every other route compared
  the whole target. The parser now splits it (`HttpRequest::query`), so
  every route matches whatever the query.
- **Bare-LF requests are accepted.** A request whose lines end in `\n`
  alone (RFC 7230 §3.5) waited out the 5 s timeout; it is parsed like
  CRLF now, mixed endings included.
- **A missing `data_dir` is created.** `budyk serve` on a fresh host
  stopped with "tier1.ring: open failed: No such file or directory"
  unless the rc.d script had made the directory. The daemon now creates
  it (one level, mode 0750, as the rc.d script does). The systemd unit
  creates `/var/db/budyk` in `ExecStartPre`, since `ProtectSystem=strict`
  with `ReadWritePaths=` refuses to start the service when it is
  missing.

- **A data race between `HttpServer::stop()` and the accept loop.**
  `stop()` closed the listening socket and wrote -1 into `listen_fd_`
  while the loop thread read the same field for every `accept4()`.
  ThreadSanitizer reported it on `test_http_server`. The field is
  atomic now and the loop reads it once. No observed misbehaviour, but
  a plain `int` shared between threads is undefined behaviour.

- **`hot_buffer.warm_grace` is applied.** The key was parsed, clamped
  and documented, and did nothing: a client connecting long after the
  last one left got a catch-up that replayed the old 1 Hz session glued
  to sparse L1 samples. Once no client (WebSocket or `/api/samples`
  poller) has been connected for `warm_grace` seconds, the hot buffer
  is now emptied (spec §3.4, M4.2). The serve loop wakes for it, so an
  L1 sleep of minutes does not delay the reset. `WarmGrace` is unit
  tested (once per idle spell, never before the first client, a
  returning client keeps the buffer, a clock step back re-anchors), and
  the new `tests/smoke/warm_grace.sh` checks it on the daemon: a client
  5 s after a 4 s session gets a 1-sample catch-up instead of 7.
- **Temp-file paths fit any `$TMPDIR`.** Alert delivery and the LLM
  client built their temp-file paths in 64-byte buffers, so a `TMPDIR`
  longer than about 45 characters made every alert fail. The buffers
  are `PATH_MAX` now, and a path that still does not fit is reported as
  `ENAMETOOLONG`.
- **Numbers are parsed whole.** `budyk tui --port`, `budyk watch-files
  --timeout` and the YAML rules' `for_ticks` and `cooldown` went
  through `atoi()`: `--port 99999` tried port 99999, `--timeout 5s`
  meant 5 ms, `for_ticks: 5s` meant 5, and `cooldown: -1` or `abc`
  dropped the field without a word. A value that is not a whole number
  in range is now an error naming it (a YAML rule with one is rejected,
  like a rule without `when`). The TUI no longer wraps an out-of-range
  level id or interval from `/api/levels` into a small number.
- **`/api/range?...&limit=1` returns the newest sample.** With a limit
  of one, each ring contributed its *oldest* record in the window, so
  `level=all&limit=1` answered with a sample from the start of the
  window. One record now means the newest, as the API documents; two
  or more still span the window end to end.

### Changed

- **Release by default, with more hardening.** A plain `cmake -B
  build` set no build type: no optimisation, no `NDEBUG`, and
  `_FORTIFY_SOURCE=2` did nothing without optimisation. The default is
  now Release (RelWithDebInfo for the sanitizer builds); a Debug build
  goes without `_FORTIFY_SOURCE`. Every build gets
  `-fstack-protector-strong` as before, plus `-fstack-clash-protection`,
  `-fcf-protection=full` and `-z relro -z now` where the compiler and
  linker take them. GCC's TSan build, now optimised, keeps
  `-Warray-bounds` as a warning rather than an error: GCC 12/13 report
  a false positive inside libstdc++'s `std::string` there.
- **No Lua tables allocated per tick.** The ten sample tables were
  created afresh twice a tick (for the custom-level conditions and for
  the rules) and left to the garbage collector (review M6). They are
  created once and updated in place now. `test_rule_perf`: median
  about 11 µs per tick (was about 13), p99 about 25 µs (was 50-70), and
  the slowest tick under 0.1 ms (was up to 1 ms) — the collector
  pauses are gone from the tail.

### CI

- **FreeBSD: the static build and the smoke scripts.** The FreeBSD
  jobs built only the port's configuration (shared) and ran the unit
  tests; the static binary the release ships was built only on a tag,
  without tests, and the smoke scripts never ran on FreeBSD. Each
  FreeBSD job now also builds the static configuration with `-Werror`,
  checks it is statically linked, runs the unit tests against it, and
  runs `serve.sh`, `crash.sh` and `warm_grace.sh` on it.
- **More clang-tidy checks.** `performance-*`, `portability-*`,
  `cert-err34-c` (string-to-number conversions that cannot report an
  error) and `cert-env33-c` (`system`/`popen`) join `bugprone-*` and
  `clang-analyzer-*`. They found the `atoi()` calls and the `popen()`
  above and one missing `reserve()`; the collectors, which `sscanf`
  kernel-formatted text and check the match count, are exempt from
  `cert-err34-c` (`src/collector/.clang-tidy`).
- **Whitespace check and `.editorconfig`.** `tests/check_whitespace.sh`
  (run by the static-analysis job) fails on trailing blanks, tabs
  outside Makefiles, carriage returns and a missing final newline;
  `.editorconfig` sets the same defaults for editors. There is no
  clang-format configuration: the code has a consistent hand-kept
  layout (aligned declarations and tables) that a formatter would
  churn, so formatting stays a review matter.
- **apt gives up on a hung mirror.** The Ubuntu jobs run apt with a
  30 s per-request timeout and three retries; a mirror that stopped
  answering used to hold a job until its `timeout-minutes` ran out.
- **One helper per unit test.** `tests/CMakeLists.txt` declares each
  test with `budyk_add_test(name LIBS ... [INCLUDES ...] [ARGS ...])`
  instead of four repeated commands; the set of tests and how they run
  is unchanged.
- **ThreadSanitizer job.** `-DENABLE_TSAN=ON` builds with
  `-fsanitize=thread`; the new `Linux TSan` job runs every unit test
  and the serve smoke test under it. The collector tick, the HTTP
  thread and the Workers share the hot buffer, the rings, the hub and
  the session store, and nothing checked those for races before. The
  smoke scripts fail on a ThreadSanitizer report as they do on ASan.
- **`-Wshadow -Wconversion -Wsign-conversion` everywhere.** The 31
  warnings they produced (implicit sign changes between `int`, `size_t`
  and `uint64_t`, `uint64_t` to `double`, one `~ECHO` into `tcflag_t`)
  are fixed with explicit casts or typed constants; none was a bug.
  Checked with GCC and with clang, which the FreeBSD jobs use with
  `-Werror`.

## [0.6.4] — 2026-10-03

A maintenance release, the rest of the code review after 0.6.3. Two
security items: failed logins are throttled per client address (HTTP
429 with `Retry-After`, no password hash computed for a blocked
attempt), and alert channel URLs — a Telegram bot token, a Discord
webhook — no longer appear in `ps` while curl runs. Start-up failures
now say what went wrong: "tier1.ring: open failed: No such file or
directory", "bind: Address already in use", "connect: Connection
refused", instead of a code. Under the hood, `main.cpp` is split into
`daemon/`, `cli/` and `web/http_util`, so every HTTP route now has a
unit test, and each helper that existed in two or three copies exists
once. `-DBUDYK_PLATFORM` is checked rather than ignored. No change to
configuration, the on-disk format or the API.

### Security

- **Failed logins are throttled.** `/api/auth/login` ran a 64 MiB
  Argon2id verification for every attempt with no limit, which made it
  both a brute-force channel and a cheap way to keep the single HTTP
  thread busy. After `web.auth.max_login_failures` failures (default
  5) from one client address within `web.auth.login_window` seconds
  (default 60), further attempts from that address get HTTP 429 with a
  `Retry-After` header until the window ends, and no hash is computed
  for them; a global window (50 failures) covers many addresses taking
  turns. A successful login clears the address. The first refusal is
  logged with the address. The dashboard shows "Too many attempts, try
  again in N s" and `budyk tui` says so instead of "wrong password?".
  `HttpRequest` now carries the client address (`peer`).
- **Alert channel secrets stay out of `ps`.** The Telegram bot token
  sits in the Bot API URL, and a Discord webhook URL is itself the
  credential; both were passed to curl as `--url` on the command line,
  readable by every local user through `ps(1)` for the duration of the
  request. Every curl run now takes its URL from a `-K` config file
  created with mode 0600 and unlinked when curl exits, the same way
  SMTP and Twilio credentials already travelled in a `--netrc-file`.
  Quotes, backslashes and control characters in the URL are escaped
  per curl's config syntax, so the URL is one value whatever it
  contains. A test runs a stand-in `curl` that records its argv and
  the files it is given: no secret or URL in argv, all of them in 0600
  files.

### Fixed

- **Start-up failures say what went wrong.** A missing or unwritable
  `data_dir` was reported as "a ring file may not match its configured
  size"; a port already in use as "HttpServer.start failed"; an
  unreadable config as "failed to load config"; the TUI printed
  "rc=-3". The modules that return small negative codes now keep the
  reason next to the code — `RingFile::describe()` and `last_errno()`,
  `TierManager::last_error()` (which ring, why, and the storage_mb hint
  only when the size really differs), `HttpServer::describe()` and
  `last_errno()`, `config_load(..., &error)` — and every message
  includes it: "tier1.ring: open failed: No such file or directory",
  "bind: Address already in use", "connect: Connection refused". The
  file-watcher messages print `strerror` instead of a number.

### Changed

- **`main.cpp` is split up; the HTTP routes have unit tests.** The
  1 300-line file held the CLI, the collector bridge, the request
  parsers, every HTTP route and the serve loop, so the daemon's most
  security-sensitive code (auth, cookies, query parsing, the login
  throttle) could only be exercised through the smoke script. It is
  now `src/web/http_util` (cookie and query parsers), `src/daemon/`
  (`router.cpp` with the routes behind an explicit `RouterDeps`,
  `serve.cpp` with the loop, signals, reload and shutdown,
  `collect.cpp` with the C → C++ bridge), `src/cli/` (the one-shot
  commands) and a `main.cpp` that only dispatches. No behaviour
  changes: the code moved as it was. `test_http_util` covers the
  parsers (cookie prefixes, saturating numbers, fallbacks) and
  `test_router` calls every route through the handler with real
  collaborators: the SPA and `/api/health`, auth disabled and enabled,
  the login flow with the throttle and the cookie, logout, the poller
  wake-up, `/api/levels`, `/api/range` parameters and the level/tier
  precedence, and the WebSocket upgrade paths.
- **One copy of each helper.** JSON escaping existed three times (alert
  payloads, LLM client, TUI login body) and unescaping twice; the
  little-endian field accessors three times (sample codec, record
  framing, ring header); `ieq`/`icontains` twice (HTTP server, WebSocket
  hub); the private temp-file writer twice (alerts, LLM), both ignoring
  `TMPDIR`; the libyaml DOM lookups twice (config loader, YAML rules
  transpiler); the realtime clock twice; the netrc builder twice inside
  `alert.cpp`; the query-string scan twice. They are now
  `core/json_text` (`json_escape`/`json_unescape`), `core/endian.h`,
  `ascii_ieq`/`ascii_icontains` in `web/http_util`, `util/tmpfile`
  (`write_private_tmp`, which honours `TMPDIR` and is 0600 from
  `mkstemp`), `util/yaml_dom`, `core/clock.h`, and one `netrc_for()`
  and one `query_find()`. The TUI's login body now escapes `\b` and `\f`
  by name instead of `\u0008`/`\u000c`, which decodes the same. Tests:
  `test_json` covers `json_escape` and a round trip, `test_codec` the
  byte layout of the endian helpers, `test_http_util` the
  case-insensitive compare, and the new `test_util` the temp file
  (`TMPDIR`, mode, failure paths) and the YAML lookups.
- **`-DBUDYK_PLATFORM` is checked, not ignored.** `cmake/platform.cmake`
  always set the platform from the host and silently shadowed the flag
  the docs and CI pass. The platform is still detected from the host
  (budyk does not cross-compile), and a flag that names a different
  platform is now a configure error. Dropped the NetBSD/OpenBSD
  branches that `src/CMakeLists.txt` rejected anyway.
- **`CLAUDE.md` describes the code as it is.** The old text promised a
  collector thread, an event loop, fully non-blocking I/O and a
  mongoose-or-libwebsockets web layer, none of which the code had. It
  now documents the real thread model (main-thread tick, one HTTP
  thread with per-connection timeouts, non-blocking WebSocket hub,
  Worker threads for alerts and `exec()`), the conventions as
  practised, the release procedure, and the known gaps against the
  spec. Removed the unused `src/web/static/index.html` stub and the
  stale "link mongoose" TODO.

## [0.6.3] — 2026-10-03

A hardening release, from a review of the code against the spec. The
theme is that nothing a rule, a client or a channel does can stop the
collector any more: a looping or memory-hungry Lua rule is cut off, an
HTTP client that stalls is closed, a dashboard that stops reading is
dropped, and alerts and `exec()` run on their own thread instead of
inside the tick. One change is visible to rule authors: `exec()` now
returns as soon as the command is queued; a rule that needs the exit
status asks for it with `exec(cmd, { timeout = 5, wait = true })`.
Also fixed: passwords containing `"` or `\` could not log in, a large
response could be cut short by a signal, child processes inherited the
daemon's sockets and ring files, and `sha256sum -c *.sha256` across a
full release download reported the Linux files as unreadable.

### Security

- **A rule can no longer stall or kill the daemon.** Until now the Lua
  engine ran rules with no limit on time or memory: a `when` with
  `while true do end` stopped collection, storage and the dashboard
  for good, and a rule that allocated without bound took the daemon
  down with an out-of-memory error — both reachable through a typo in
  `rules.lua` and a SIGHUP. The engine now counts VM instructions with
  a Lua hook (`lua_sethook`, spec §3.6) and allocates through a
  budgeted allocator (`lua_newstate`). Each call into Lua — a rule's
  `when` or action, a custom level's `when`, the rules file being
  loaded — may run at most `rules.limits.instructions` instructions
  (default 1 000 000, about 10 ms), and the whole engine may hold at
  most `rules.limits.memory_mb` (default 16). An overrun raises a Lua
  error in that call only: it is logged once like any other rule
  error, the rule is skipped on that tick, and the other rules and the
  next tick run as usual. Instructions are counted rather than time so
  the limit means the same on a slow machine or under a sanitizer.
  `test_lua_engine` cases 32–34 cover a looping `when`, a looping
  action, a loop at file level, unbounded allocation, a single huge
  `string.rep`, and a looping level condition. The hook costs nothing
  measurable in `test_rule_perf`.

- **Alerts and `exec()` no longer block collection.** Both ran inside
  the collector tick: each alert channel is a curl run of up to 20 s,
  one after another, and an `exec()` waited for its command, 30 s by
  default and up to a day. While they ran, nothing was collected,
  stored or broadcast, and the dashboard froze. A new `Worker` (one
  background thread with a bounded queue of 256 jobs) carries that
  work. `alert()` queues the alert and returns; the dispatcher's
  thread sends it and logs failures, and `AlertDispatcher::dispatch`
  now reports the number of channels queued. `exec()` queues the
  command and returns `{ queued = true }`; its outcome (exit status,
  signal, timeout) goes to the log. `exec(cmd, { timeout = 5, wait =
  true })` keeps the old inline behaviour and result table for a rule
  that needs the exit status; that timeout is capped at 60 s. A full
  queue drops the job and logs it. On shutdown the daemon gives queued
  alerts 5 s and a running command 2 s, then cancels the rest
  (`exec_command` takes a cancel flag and kills the child), so a stuck
  channel cannot hold the stop. A reload keeps both queues. Spec §3.6:
  "exec(): fork+exec, non-blocking".
- **A stalled HTTP client no longer holds the server.** The embedded
  server handles one connection at a time on one thread, and a client
  that opened a connection and sent nothing, or dripped its request a
  byte at a time, kept every other request — the login included —
  waiting for as long as it liked. Each connection now has a budget
  (`HttpServer::set_io_timeout_ms`, 5 s): socket timeouts bound each
  read and write, and a deadline bounds the whole request, so a
  stalled client is closed without a response and the next one is
  served. The listen backlog grows from 16 to 64. Tests cover the
  quiet client, the dripping client and a body that never arrives.
- **A WebSocket client that stops reading is dropped, not waited on.**
  The hub sent every frame with a blocking `send(2)` under its lock, so
  one dashboard whose machine went to sleep (a half-open connection, or
  a full socket buffer) blocked the collector tick — no samples
  collected, stored or broadcast — until TCP gave up, minutes later.
  Client sockets are now non-blocking, and a send that would block or
  writes only part of a frame drops the client (spec §3.3 item 4).
- **The hub reads from its clients.** It never did, so close frames
  and pings were ignored and a client that vanished kept the client
  count up, pinning the scheduler at L3. Once per tick the hub now
  answers pings with pongs, answers close frames and drops the client,
  drops a client that hung up or sends something that is not a
  WebSocket frame, and skips data frames. A dashboard that vanishes
  releases L3 on the next tick: measured on the daemon, the level goes
  back to L1 within the grace period after the client's connection is
  reset.
- **Child processes no longer inherit the daemon's descriptors.** Rule
  `exec()` and the alert channels fork; the listening socket, client
  connections, the ring files and the sessions file were open in every
  child. A hung child kept the port bound after a restart and could
  write into a ring the next instance had already reopened. All of
  them are now close-on-exec (`SOCK_CLOEXEC`, `accept4`, `O_CLOEXEC`),
  and the serve smoke test checks every descriptor the daemon holds
  (Linux, via `/proc/<pid>/fdinfo`).
- **`/api/health` no longer reports `data_dir`.** The endpoint is
  unauthenticated so liveness probes work; a path on the host is not
  something a probe needs.

### Fixed

- **Passwords containing `"` or `\` can log in.** The login body was
  read without decoding JSON escapes, so a password the dashboard
  sends as `pa\"ss` arrived truncated and was rejected. The body is
  now decoded (`json_get_string` / `json_unescape` in `web/json.cpp`,
  with tests), including `\uXXXX`, so a Cyrillic password sent as
  escapes works too.
- **Large responses are no longer cut off by a signal.** The HTTP
  server sent a response with one `send(2)` per part and treated a
  short write as failure. A blocking send of a multi-megabyte
  `/api/range` body waits on the client, and a signal that lands then
  (SIGHUP is routine) makes it return the partial count; the client
  got a truncated body with a full `Content-Length`. The server now
  sends until everything is written (with `MSG_NOSIGNAL`, so a client
  that hung up is a failed send, not SIGPIPE). The test reproduces the
  interruption with an interval timer aimed at the server thread.
- **Expired web sessions are purged every minute.** They were only
  dropped on the next login or request, so a quiet daemon kept every
  past login in memory and in `sessions.tsv`.
- **One source of truth for the version.** `budyk version` and
  `/api/health` take it from `project(VERSION)` in CMake
  (`BUDYK_VERSION`) instead of two string literals in `main.cpp`.
- **`sha256sum -c *.sha256` works on a full set of release assets.**
  The FreeBSD sidecars were written by `sha256 -r` (one space between
  hash and name), the Linux ones by `sha256sum` (two spaces). Given
  both in one run, GNU `sha256sum -c` misread the Linux lines and
  reported them as unreadable; each file checked on its own was fine.
  FreeBSD sidecars are now in the GNU format too, and a new
  `checksums` job in the release workflow runs `sha256sum -c` over all
  of them at once, on the dry run as well, before anything is
  published.

### Tests

- **Rule evaluation time is measured (spec §5 item 13).**
  `test_rule_perf` loads 100 `watch()` rules shaped like those in
  `rules/examples.lua` (thresholds, computed thresholds, sustain
  counters, cooldowns, a quarter of them firing now and then) and times
  2000 ticks. It fails if the median tick exceeds 1 ms; sanitizer and
  coverage builds only report the figure. Measured: median about 13 µs
  per tick, p99 about 70 µs.
- **Crash test (spec §5 item 7).** `tests/smoke/crash.sh` runs the
  daemon with a 0.2 s custom level, kills it with SIGKILL at a random
  moment while a WebSocket client reads samples, and restarts it on the
  same data directory, eight times. After each restart every sample the
  client saw before the kill must be in the ring (the last one may be
  missing: it can be pushed before it is stored), ring timestamps must
  be strictly increasing, and the log must have no sanitizer report.
  It runs in the ASan + UBSan and coverage jobs. Measured: no sample
  lost in any round. A deliberately broken `RingFile::append` (some
  `write_idx` increments skipped) fails every round.

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

[0.6.4]: https://github.com/click0/budyk/releases/tag/v0.6.4
[0.6.3]: https://github.com/click0/budyk/releases/tag/v0.6.3
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
