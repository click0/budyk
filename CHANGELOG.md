# Changelog

All notable changes to budyk will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- **The release procedure** (`CLAUDE.md`, the comment in `release.yml`):
  the usual way is a release published by hand on GitHub with a new tag
  on `main`, which the workflow completes with the files and the notes,
  as it did for 0.8.1 and 0.8.2. The note in 0.8.2 that the workflow
  cannot complete such a release was wrong; what to do when GitHub
  refuses the update (v0.6.5) is described.
- **The network totals count the devices only.** A container's packet
  goes through its veth, the bridge and the uplink, and a jail's through
  its epair, the bridge and em0: each counted it, so on a container or
  VM host `net.rx_bytes_per_sec` / `tx_bytes_per_sec` read up to three
  times the traffic. Now only a device's interface counts (a NIC, Wi-Fi,
  a VM's virtio; on Linux one with `/sys/class/net/<if>/device` and not
  enslaved to another device, as Azure's SR-IOV VF is to eth0, whose
  counters have its traffic; on FreeBSD Ethernet or 802.11 and not made by a cloner, wlan aside); a
  veth, bridge, bond, VLAN, tun/tap, epair or WireGuard interface does
  not. With no device at all, as inside a container, every interface
  counts, as before. `net.interfaces` still lists them all. On such
  hosts the totals, and rules on them, now read lower.

### Fixed

- **Any website could read an unprotected budyk on 127.0.0.1** by DNS
  rebinding: a page points a name of its own at 127.0.0.1 once it has
  loaded, and the browser then takes budyk for that site, so the page
  reads `/api/samples`, the alerts and the WebSocket as its own origin
  (the `Origin` check of 0.8.2 passes too). The default configuration
  (the one the Docker image runs) and an SSH tunnel to the dashboard
  were open to it. Without a password, on a loopback `listen`, a request's
  `Host` must now be an IP address, `localhost` (or `*.localhost`), the
  host's name or one in the new `web.allowed_hosts`; anything else gets
  403. A reverse proxy that passes its public name in `Host` to an
  unprotected budyk needs that name in `web.allowed_hosts`.
- **FreeBSD: swap past the 15th device was not counted.** The swap
  total and use summed the per-device entries of `kvm_getswapinfo()`,
  which fills at most one fewer than its array holds (16): a host with
  more swap devices showed only the first 15. The total of all devices,
  which libkvm adds after them, is used now.
- **A storage server's disks past the 64th were in no list.**
  `disk.devices`, `net.interfaces` and `fs.mounts` (the dashboard and
  the rules) held 64 entries each: on a host with 90 drives, or a
  container host with hundreds of interfaces, the rest were left out
  without a word. They hold 256 now.
- **The disk and network totals followed 256 devices.** Since each
  device's own delta is summed (0.8.2), a device past the 256th moved
  nothing: on a host with 300 container interfaces the network total
  missed 44 of them. They follow 1024 now, each found where the last
  one was rather than searched from the start (half a million string
  comparisons a tick at a thousand devices).
- Past either limit the log says so once, with the count:
  "budyk: 300 disks: disk.devices lists the first 256".
- **The Docker files did not work.** `docker-compose.yml` failed its
  own schema check (`dockerfile:` outside `build:`), so `docker compose
  up` started nothing; the image's build stopped on CMake not finding
  Alpine's `liblua` (in `/usr/lib/lua5.4`), or, in a tree with a local
  `build/`, on that tree's `CMakeCache.txt`; the image lacked
  `libstdc++`, so `budyk` did not start; and the compose file mounted
  `config.yaml` while the image read its example config. All four are
  fixed (`.dockerignore` keeps local build trees out), and CI now builds
  the image and runs it with `docker compose` on a config of its own.

## [0.8.2] — 2026-10-25

Fixes from a review of every module. Three of them matter most to a
packaged install. Under the systemd unit every filesystem but the data
directory's was skipped, so a full `/` never raised an alert. A ring
created on a full disk crashed the daemon (SIGBUS) and left a file that
stopped every later start. And any web page open in the browser could
read the WebSocket stream: the host name, the processes, the mounts and
the alerts. Also fixed: a rule can no longer abort the daemon or run
past its instruction limit; a client can no longer stop or hold the
HTTP thread; a WebSocket now ends with its session; and the config reads
`5m` as five minutes, not five seconds. The record format does not
change: the rings of 0.8.1 are used as they are. Behind a reverse proxy
that rewrites `Host`, the proxy must now pass `X-Forwarded-Host` (or
the original `Host`) on. Otherwise the dashboard's WebSocket is refused
with 403.

### Changed

- **A release can be published from Actions** (`release.yml`): Run
  workflow on `main` with `tag` = `vX.Y.Z` checks that `CMakeLists.txt`
  and `CHANGELOG.md` are at that version and the tag is new, builds
  everything, and publishes the release, creating the tag on the commit
  it built. No tag needs to be pushed by hand, and no release made by
  hand first (which the workflow cannot complete). A tag push still
  works, and is now checked against `CMakeLists.txt` too.

### Fixed

- **A rule could abort the daemon.** Each tick's bind of the sample ran
  outside any protected call, so a Lua error there was a panic: a rules
  file with a "strict globals" guard on `_G` (a `__newindex` that
  raises) aborted on the first tick, and so did a rule that kept the
  state at `rules.limits.memory_mb` (a history table it never trims)
  once the bind needed memory. The bind is now protected and sets the
  globals raw; when it fails the tick goes without rules, logged once.
- **`pairs()` over a sample table handed out the table behind the
  read-only proxy**: a rule could change what the next rule read, or
  give it a metatable the next bind would run. It now hands out the
  proxy.
- **`watch()` read `message` and `when` twice**: an `opts` with an
  `__index` that answered a table the second time crashed the load.
  Each field is read once. `exec()` builds its result table only after
  its C++ objects are gone, and formats a number argument itself.
- **The rule state file on a full disk**: a write that failed was
  renamed over the good file, and every cooldown reset on the next
  start. Write errors are checked (and the file synced) before the
  rename, and a failed save is logged with its reason.
- **YAML rules**: a control character before a digit in a name or
  message became another character (`\1` then `23` read as `\123`).
- **The instruction limit could be dodged, and the tick then waited
  forever.** There were three ways:
  - a `pcall()` around a loop caught the limit's error and the loop
    went on;
  - an `xpcall()` message handler and a `__gc` finalizer run with Lua's
    hooks off;
  - several library calls run in C, where the count hook sees one
    instruction for the whole call: a backtracking pattern
    (`string.find(s, "a*a*a*b")` on a long string), a plain `find` with
    a long needle, `string.rep("", n)`, `table.move` over a huge range,
    and the table functions over a `__len` or a C `__index`.

  Now:
  - past the limit every instruction raises;
  - `xpcall` calls its handler after the call;
  - `setmetatable` refuses `__gc`;
  - `find`, `match`, `gmatch` and `gsub` are Lua 5.4.7's own,
    vendored, and charge their steps to the limit;
  - `rep("")` returns at once and `table.move` is charged;
  - `table.concat`, `insert`, `remove` and `sort` refuse a table with
    `__len`, `__index` or `__newindex`.
- **The `exec()` allowlist on a multi-call binary.** It compared the
  command's realpath, and on BusyBox (the Alpine image) `/bin/sh`,
  `/usr/bin/logger` and every other applet are the same file: allowing
  one allowed them all, each run as itself. The name as written must
  now match the allowed entry's too. A link to an allowed binary under
  another name is no longer allowed.
- **The file watcher went blind after the first save by rename.**
  inotify and kqueue watch an inode, not a path. vim, visudo, `sed -i`
  and package managers write a new file and rename it over the old one,
  and after the first such save every later change to the path was
  missed, while the path still counted as watched. A path whose file is
  deleted or replaced is now watched again as soon as it exists (Deleted
  then Created, usually in the same poll). Two paths to one inode (hard
  links) both get the events, instead of the second taking the watch
  over. If inotify's queue overflows, every path is reported changed and
  each watch is checked again, with one line logged. `watched_count`
  counts the paths watched right now.
- **A line break in a rule or alert name went into a header** (ntfy's
  `Title:`, the mail's `Subject:`), adding headers of its own: a `Bcc:`,
  any ntfy header. A name can carry anyone's bytes, such as a process
  name or a mount point. Control characters in header values are now
  spaces.
- **One failed accept() stopped HTTP for good.** The HTTP thread ended
  on any accept error but EINTR: enough WebSocket clients to reach the
  descriptor limit (EMFILE, about 4 090 under the packaged unit, no
  login needed with auth off), or on FreeBSD a client that reset before
  the accept (ECONNABORTED). The loop now retries, logged once, and
  WebSocket clients are capped at 64 (503 past that).
- **A WebSocket outlived its session.** The upgrade checked the session
  once: after a logout, or once the session expired, the stream went on.
  The hub now closes (1008) a client whose session ended, on the next
  tick, and the dashboard goes back to the login form.
- **Any website could read the stream.** A WebSocket upgrade with an
  `Origin` that is not the `Host` it was sent to (or a proxy's
  `X-Forwarded-Host`) is refused with 403. A page on another site, open
  in the browser, used to get the host name, the processes, the mounts
  and the alerts, and held the daemon at L3. A reverse proxy that
  rewrites `Host` must pass `X-Forwarded-Host` on (see the README).
- **A slow reader held the HTTP thread.** The send timeout restarts with
  each send, so a client reading a little within each one kept the
  thread as long as it liked. A response now gets the I/O timeout plus
  its size at 256 KiB/s.
- **Under the packaged systemd unit, filesystems never reported full.**
  The read-only test used the mount's options in budyk's own mount
  namespace, and `ProtectSystem=strict` makes every mount read-only there
  except the data directory: `/`, `/srv`, `/var/lib/...` were skipped.
  It now reads `/proc/self/mountinfo` and goes by the filesystem's own
  options.
- **A process could hide from the counts and the top list** by putting a
  newline in its name (one `prctl()`): `/proc/PID/stat` was read up to
  the first newline, which then had no `)`. The whole file is read now.
- **The total disk and network rates jumped when a device came or went.**
  The rates were the delta of the summed counters. An interface
  arriving with its counters (a jail's vnet back on the host, a moved
  netns) showed as gigabytes per second for one tick, and one leaving (a
  container's veth) showed 0 while the others moved. Each device's own
  delta is now summed, and a new one starts from a baseline.
- **UDP over IPv6 read 0 on Linux.** IPv6's counters are kept apart, in
  `/proc/net/snmp6`, and are now added.
- **A collector that failed passed zeros off as readings**: for example
  `proc.total = 0` when `kern.proc.proc` failed during a fork storm. Its
  groups are now left out of the sample (logged when it starts failing),
  and a group read again starts from a baseline.
- **On-demand mode: the first sample after an idle gap** had CPU, disk
  and network rates averaged over the whole gap (70 % CPU on an idle
  box, from the minutes before). A tick that reads nothing now makes the
  next read a baseline.
- **A full disk crashed the daemon and blocked every later start.** A new
  ring's header was written through the mapping of the sparse file, which
  is SIGBUS once the page cannot be allocated. That left a full-size ring
  with a blank header, and every start after it failed with "bad magic".
  The header now goes in with `pwrite` (an error, and an empty file left,
  if it fails), and a ring with a blank header is made anew.
- **On a full disk the history stopped without a word.** A store that
  fails is now logged when it starts and when it stops, with the ring
  and the reason: "tier1.ring: writing a record failed: No space left on
  device".
- **After the clock stepped back further than a ring spans,** each start
  counted the records from before the step as written past the index:
  they were stamped later than the newest. The writes after them then
  overwrote the real records. Records stamped after now are no longer
  counted.
- **A query during a store could return a record out of order.** A
  query walks a full ring from the newest record to the oldest. A store
  during the walk overwrote the oldest slot before the walk read it, and
  that new record came first in the oldest-first result. The walk now
  stops where the writer has been.
- **A SIGHUP with a broken rules file dropped the working rules.** The
  engine was shut down before the new file was loaded, so a typo at rule
  200 of 300 left 199 rules running. The file is now loaded into a
  scratch engine first, and one that does not load keeps the rules as
  they were (logged).
- **A SIGHUP ended the sleep**, and the tick right after the previous one
  stored rates over a few milliseconds. The reload is now applied inside
  the sleep, which goes on.
- **Config numbers were read up to the first non-digit.** `interval: 5m`
  was 5 seconds, `1.5h` was 1, `4294967596` was 300 and `nan` made a
  threshold that never fires. Numbers are now read whole. Durations
  take a unit or none (300, 300s, 5m, 1h, 1d; 500ms for a level's
  interval), as the spec writes them. A value that does not read is
  reported under its full name, and the default is kept.

## [0.8.1] — 2026-10-22

Two fixes in the readings, found by two new tests that hold budyk to
the spec. On FreeBSD the process count was the thread count and the
thread count its square (105 and 1561 where `ps` shows 42 and 106). On
a busy host the total disk and network rates were wrong once an
interval moved more than 18.4 GB, which at the 5-minute heartbeat is
61 MB/s. CI now compares every reading with the system's own tools
(`free`, `swapinfo`, `ps`, `df`, `ss` / `netstat`, ...) on Linux and
FreeBSD amd64 and arm64, and checks the spec's resource figures: on a
CI runner the release build uses 3.5 to 4.5 MB of memory and 0.1 % of a
CPU with a dashboard open. The record format does not change: the rings
of 0.8.0 are used as they are.

### Fixed

- **FreeBSD process counts.** `proc.total` counted threads and
  `proc.threads` grew as the square of each process's thread count: the
  collector walked `kern.proc.all`, which has an entry per thread, and
  summed every entry's `ki_numthreads`. On a FreeBSD 14.4 VM that was
  105 processes and 1561 threads where `ps` shows 42 and 106, and a
  multi-threaded process reached the top-process tracker once per
  thread. It now walks `kern.proc.proc`, one entry per process, as `ps`
  without `-H` does. Found by the new cross-check below.
- **Disk and network totals on a busy host.** The total read, write,
  receive and transmit rates were computed as delta x 1e9 / ns in 64
  bits, which overflows once an interval moves more than 18.4 GB: at the
  5-minute L1 heartbeat that is 61 MB/s, so a busy server showed a wrong,
  much smaller rate. They are now computed in double, as the
  per-device rates already were (`budyk_rate_u64()`), on Linux and
  FreeBSD.

### Added

- **The readings are checked against the system's own tools**
  (`tests/smoke/crosscheck.sh`, spec M2 / M3), in CI on Linux (ASan,
  coverage), FreeBSD 14.4 and 15.1 and the emulated FreeBSD arm64: CPUs
  and per-core entries; memory and swap (`free -b`; `vm.stats.vm` and
  `swapinfo -k`); load and uptime; processes and threads (`ps -A`); TCP
  established (`ss`; `netstat -an`); each filesystem's size and use
  (`df -P -k`); the interfaces (`/sys/class/net`; `ifconfig -l`) and
  whole disks (`/sys/block`; `kern.disks`); a busy loop near 100 % of
  one CPU in the top processes and in the CPU total; a 64 MB fsync'ed
  write seen in the disk write rates.
- **The resource figures of spec §5 are tested**
  (`tests/smoke/resources.sh`, in CI on the Linux static build and on
  FreeBSD 14.4 and 15.1): idle for 60 s, RSS at most 10 MB, CPU at most
  0.5 % of one CPU, under 1 KB a minute to disk; with one client at L3
  for 30 s, RSS at most 25 MB and CPU at most 1 %; the first L3 sample
  within 2 s of the connect; the catch-up of a full hot buffer (300
  records) within 100 ms; the stripped static amd64 binary at most 5 MB.
  The release build on a CI runner: 3.5 MB idle, 0.13 % at L3, L3 after
  4 ms, the catch-up in 11 ms, 3.4 MB on disk.

## [0.8.0] — 2026-10-19

Rules work as spec §3.6 describes: `cooldown` defaults to `for_ticks`,
a `when()` that returns `nil` skips the tick without losing the streak,
`action` can be a list, and alerts reach the dashboard (a WebSocket
frame, an Alerts panel and `/api/alerts`). New readings: the top
processes by CPU and by memory (live), ICMP counters, and the host's
name. A release now carries Linux `.deb` and `.rpm` packages and
FreeBSD aarch64 binaries, and the FreeBSD builds move to 14.4 and 15.1.
budyk cross-compiles for FreeBSD on Linux, and `--target docs` builds an
API reference with Doxygen. Fixed: budyk now runs on arm64 (an
unaligned atomic in the ring header raised SIGBUS), a descriptor could
leak into an alert or `exec()` child, and a process name that is not
UTF-8 broke the dashboard. **Upgrade notes:** a record grows to 542
bytes (codec v9), and the rings of an older budyk are converted when the
daemon first starts; a 0.7 daemon does not open the converted rings, so
to go back, move them aside. A rule that relied on the old `cooldown`
default of 0 now fires once and then waits as long as it took to fire;
set `cooldown = 0` for the old behaviour. The FreeBSD tarballs are named
`freebsd14.4-amd64`, `freebsd15.1-amd64` and `freebsd14.4-aarch64`.

### Added

- **API reference** (spec §5 item 12): `cmake --build build --target docs`
  runs Doxygen over the headers in `src/` (HTML in `build/docs/html`). The
  header comments that describe a declaration are now Doxygen comments
  (`///`, `///<`, `/// @file`). Any warning fails the target. CI builds it
  and keeps the HTML as the `api-reference` artifact.
- **Cross-compiling for FreeBSD** (spec §5 item 8): on a Linux host with
  clang and lld, `cmake --toolchain cmake/toolchains/freebsd-amd64.cmake`
  (or `freebsd-aarch64.cmake`) with `-DFREEBSD_SYSROOT=DIR` builds the
  FreeBSD binary, static or shared, tests included.
  `cmake/toolchains/freebsd-sysroot.sh VERSION ARCH DIR` makes the
  sysroot from the release's `base.txz` and the `lua54`, `libyaml` and
  `libargon2` packages, and checks every download: `base.txz` against the
  release's `MANIFEST`, the package catalogue's signature against the pkg
  key in `base.txz`, each package against the catalogue. CI builds both
  for 14.4, runs clang-tidy on the FreeBSD side of the code (until now
  left out) and runs the smoke scripts with the cross-compiled amd64
  binary on FreeBSD 15.1.
- **FreeBSD aarch64 release binaries**: `budyk-<version>-freebsd14.4-aarch64`
  (and `-debug`), cross-compiled, and run through the smoke scripts in an
  emulated arm64 FreeBSD 14.4 before the release is published.
- **Linux packages** (spec M9): a `.deb` and an `.rpm` of the static
  binary in every release, built by `addons/linux/mkpkg.sh`. They install
  `/usr/bin/budyk`, `/etc/budyk/config.yaml` and `rules.lua` (conffiles,
  Linux paths: data in `/var/lib/budyk`), the systemd unit, the man page
  and the example rules; create the system user `budyk`; leave the
  service to `systemctl enable --now budyk`. CI installs both (the .rpm
  in a Fedora container), runs the daemon as `budyk`, under systemd for
  the .deb, and removes and purges them (`addons/linux/test-packages.sh`);
  the release job does the same before publishing.
- **ICMP** (spec §2.1, "TCP/UDP/ICMP counters"): messages in and out,
  errors in and destination unreachable received, per second, over
  IPv4 (Linux: the `Icmp:` pair of `/proc/net/snmp`; FreeBSD:
  `net.inet.icmp.stats`). A new group `icmp`, read with TCP and UDP, in
  the rings, the API, the rules, the dashboard and the TUI; folded
  records average it.
- **The host's name** (spec §2.1): `/api/health` gives `"hostname"`, and
  the dashboard and the TUI show it in their title.
- **The top processes, live** (spec §2.1): the
  10 that use the most CPU and the 10 largest by memory, each once,
  busiest first, with pid, name, state, threads, CPU % (of one CPU over
  the last interval) and RSS. Linux reads utime + stime from
  `/proc/<pid>/stat`, FreeBSD `ki_runtime` from `kern.proc.all`; a pid
  reused by another process (another start time) starts from 0; kernel
  threads (and FreeBSD's system processes) are left out, as `top` does.
  The dashboard has a Processes table, the TUI lists as many as fit,
  rules get `proc.top`, and the detail JSON `"processes"`. A new list for
  the metric sets, `proc.top`, is in `full`. Not stored, like the rest of
  the detail.
- **Alerts reach the dashboard** (spec §3.6.4: "alert: log + WS
  event"). Every alert, from a rule's `alert` action or from `alert()`
  in Lua, is written to the daemon log ("budyk alert: critical
  disk_full: /var at 97%"), pushed to open dashboards as a WebSocket
  frame `{"count":N,"alerts":[...]}`, and kept for the new
  `GET /api/alerts` (the last 100, in RAM). The dashboard lists the
  latest 20 under the live panel; the rule names and messages go in as
  text. An alert used to reach only the configured channels, and with
  none configured it left no trace.
- **Action lists** (spec §3.6.2): `action = { alert, escalate }` runs
  each in order; one that fails is logged and the next still runs.
  `escalate` (or `"escalate"`) as an action keeps L3 on for 60 s.
- **`exec()` as an action.** Written in the rules file itself, outside
  `when` and the actions, `exec("/path")` runs nothing: it checks the
  command (enabled, absolute, no `..`, the allowlist) and returns it as
  an action, so `action = { alert, exec("/usr/local/bin/drop-caches.sh") }`
  works as the spec writes it. A bad command fails the load. It is
  checked again when it runs.
- **`tests/unit/test_rule_files`** loads `rules/examples.lua` and
  `rules/freebsd-defaults.lua` and runs them, so the shipped examples
  keep working.

### Changed

- **FreeBSD 14.4 and 15.1** replace 14.2 and 15.0 (both past their end of
  life) in CI and in the release: the tarballs are
  `freebsd14.4-amd64` and `freebsd15.1-amd64`, built on the oldest
  supported release of each branch, so they run on every later one. With
  the aarch64 tarballs and the Linux packages a release has 20 files.
- **Records grow to 542 bytes (codec v9)** for ICMP. A ring of 0.7
  (502-byte records) or 0.6 (270) is converted when the daemon starts,
  as on the 0.7 upgrade; `tests/smoke/upgrade.sh` now runs against the
  0.7.0 release too. A 0.7 daemon does not open the converted rings.

- **`cooldown` defaults to `for_ticks`** (spec §3.6.4): a rule that keeps
  holding fires once, stays quiet as long as it took to fire, then may
  fire again. It used to default to 0 and fire on every tick. Set
  `cooldown = 0` (or `cooldown: 0` in YAML) for the old behaviour.
- **`when()` returning `nil` skips the tick** (spec §3.6.5): the streak
  toward `for_ticks` is kept, as for a group the level does not collect.
  `return d and d.busy_percent > 90` therefore waits out a device that
  is briefly gone. A `when` that returns nothing at all still counts as
  false and resets the streak, as `false` does.
- **`for_ticks` and `cooldown` must be whole numbers** (from 1 and from 0,
  up to 1 000 000); anything else fails the load with the rule's name.
  `for_ticks = 2.5` used to read as 0, that is 1, and `cooldown = "x"`
  as 0. `for_ticks = 0` still means 1.

### Fixed

- **budyk runs on arm64.** The ring file's write index sits at byte 28 of
  its header, not 8-byte aligned, and was read and advanced there with
  atomic instructions: x86 allows that, arm64 raises SIGBUS, so the
  daemon died on its first ring and the ring unit tests failed. The
  index is now an atomic in memory, copied into the header after each
  write; the file format does not change. GCC builds add
  `-Wcast-align=strict`, which turns such a cast into a build error on
  x86 too, and CI runs the unit tests and the smoke scripts on FreeBSD
  arm64.
- **No descriptor leaks into a forked child.** The Linux collectors, the
  config, rules, freeze and session files and the private temp files
  for curl were opened without close-on-exec. They are open for a moment
  only, but the alert and `exec()` workers fork from other threads, and
  a child forked in that moment kept the descriptor through `exec`
  (curl, or a rule's command, holding `/proc/<pid>/stat` or a temp
  file). They are now `fopen(..., "re")` / `mkostemp(..., O_CLOEXEC)`.
  The descriptor check in `tests/smoke/serve.sh` looked once, so it
  saw a collector's file only by luck; it now reads the descriptors
  over and over for 3 s.
- **A name that is not UTF-8 no longer breaks the dashboard.** JSON
  strings carried a process name, a mount point or a rule's message as
  bytes; one that is not UTF-8 made the WebSocket frame invalid, and a
  browser must drop the connection on such a frame. Each byte that
  starts no well-formed UTF-8 sequence is now `\ufffd`. Anyone on the
  host can name a process.

## [0.7.0] — 2026-10-17

Adaptive collection as the spec describes it. Each level reads its own
set of metrics (`collection.<level>.metrics`), `collection.mode` adds
`always` and `on-demand`, and a sustained anomaly now escalates from L2
to L3. While L3 or a custom level runs, the L2 and L1 rings keep
filling with folded summaries, so the long history has no holes. The
metric set grows to CPU time by kind, processes by state, TCP and UDP,
filesystem usage and clock sync, plus live per-core, per-disk,
per-interface and per-filesystem detail on the dashboard and in rules.
Rules skip a tick when their level does not collect what they read,
instead of seeing zeros. New: `budyk fsck`; a crash no longer loses the
last stored record; one daemon per data directory. Opening the
dashboard no longer records a false CPU spike. **Upgrade note:** the
new fields make a record 502 bytes (codec v8), and the rings of an
older budyk are converted when the daemon first starts. A 0.6 daemon
does not open the converted rings, so to go back, move them aside. The
defaults collect every metric at every level, so an unchanged config
behaves as before.

### Added

- **Per-level metric sets** (spec §3.4.4). `collection.<level>.metrics`
  (and `metrics:` on a custom level) says what a level reads: `minimal`
  (cpu, mem, swap, load, uptime: spec §3.4.1's heartbeat), `standard`
  (every group), `full` (also `cpu.cores`, `disk.devices`,
  `net.interfaces`, `fs.mounts`) or a list of names. The defaults, L1
  `standard` and the rest `full`, read every metric, as before.
  A tick reads the set of the level it slept at, or of the level a
  client or a request has raised it to since; the collectors a set
  leaves out do not run. Each sample says what it has: `groups` in the
  JSON (and in the record, since codec v8), `"collected"` in the detail,
  `metrics` and `collects` per level in `/api/levels`. The dashboard and
  the TUI mark the rest "not collected at this level", and the history
  chart leaves a gap. A rate needs two reads in a row: on the daemon's
  first tick and on a level's first tick after one that did not read it,
  its group is left out instead of reading 0.
- **Rules skip what their level does not collect** (spec §3.6.5). A
  `when` that reads a missing group or list is skipped for the tick,
  its `for_ticks` count kept; a level that does not collect it is logged
  once per rule ("rule 'io' skipped while the level does not collect
  what it reads: rules.lua:3: disk is not collected at this level"), a
  rate's first read is not. `collected("disk")` tells a rule what the
  sample has. A custom level's `when` that reads something missing is
  false.
- **`collection.mode`** (spec §3.4.4): `adaptive` (the default),
  `always` (L3 all the time; the L2 and L1 rings get folded records) or
  `on-demand` (collect only while a dashboard or TUI is connected, plus
  `grace_period`, or a custom level is active; thresholds raise
  nothing, L1 and L2 collect nothing, nothing is written to the rings).
  The new `tests/smoke/modes.sh` checks the sets and both modes on the
  daemon.

- **Each core, disk, interface and filesystem, live** (spec §2.1). The
  dashboard gets per-core bars and tables of disks (read / write rates,
  operations, busy %), interfaces (rates, packets, errors, drops, down
  ones in red) and filesystems (used, size, inodes); rules get them as
  read-only collections `cpu.cores`, `disk.devices`, `net.interfaces`
  and `fs.mounts`. Linux reads them from the same `/proc/stat`,
  `/proc/diskstats` (busy from `io_ticks`), `/proc/net/dev` and mounts
  as the totals; FreeBSD from `kern.cp_times`, devstat (busy from
  `busy_time`), `getifaddrs` and `getmntinfo`. A device that appears
  starts from a baseline; one that goes away is dropped. The detail is
  not stored in the rings: the WebSocket frame and `/api/samples` carry
  the newest sample's as `"detail"`.

- **More metrics** (spec §2.1), on Linux and FreeBSD, in the rings, the
  API, the rules, the dashboard (new rows and two history charts) and
  the TUI:
  - `cpu.user_percent`, `system_percent`, `iowait_percent`,
    `irq_percent`, `steal_percent`, `idle_percent`: where the time
    went (FreeBSD has no iowait or steal);
  - `proc.sleeping`, `blocked`, `stopped`, `zombie`, `threads` next to
    `total` and `running`;
  - `tcp`: sockets established and in TIME_WAIT, and per-second
    active and passive opens, retransmits, segments in and out, input
    errors (Linux `/proc/net/snmp` and `sockstat`, FreeBSD
    `net.inet.tcp.stats` and `.states`);
  - `udp`: datagrams in and out, to no port, input errors per second;
  - `fs`: how many local filesystems and the fullest by space and by
    inodes (network, FUSE, pseudo and read-only mounts are left out: a
    hung server must not block the collector tick);
  - `clock`: whether the kernel considers the clock synchronised, its
    offset and maximum error (`ntp_adjtime`).
  Folded L2 / L1 records average the rates and keep the maxima of the
  counts and the fullest filesystem.
- **Rings from 0.6 are converted on start.** The new fields make a
  record 502 bytes (was 270; codec v8). A ring written by an older
  budyk is rewritten at the new size when the daemon starts, the newest
  records first if the size budget now holds fewer, and the log says
  so ("tier1.ring: converted from 270-byte records (an older budyk) to
  502: 3883 record(s) kept, 0 dropped"). The rewrite goes through
  `<ring>.convert` and a rename, so a crash half-way leaves the old
  ring. A ring from a newer budyk is left alone, with an error that
  says so. `budyk fsck` reports an older ring as a note.

- **`budyk fsck [--repair] [--config FILE] [DATA_DIR]`** (spec M4).
  Checks every ring in the data directory: the header, then each record
  (CRC, level, time order), and reports per ring the records, their time
  span and any problem: damaged records (readers skip them; nothing to
  repair), records of another level, a clock that stepped back (a note:
  a query across that point may miss records). `--repair` counts in the
  records written past `write_idx` and never counted, which a crash
  between a record's `pwrite` and the index update leaves. Exit status
  as fsck(8): 0 clean, 1 repaired, 4 problems left, 8 the check could
  not run.
- **A daemon crash loses no stored record.** On start, `TierManager`
  counts in such records itself and the daemon logs it
  ("level-burst.ring: 1 record(s) written before a crash and not yet
  indexed, now counted in"). It used to lose the last record.
- **One daemon per data_dir.** `budyk serve` takes an flock on
  `<data_dir>/budyk.lock` (with its pid in it) and a second one on the
  same directory stops with "data_dir '...' is in use by another budyk
  (pid N)". Two daemons used to write into the same rings. `fsck
  --repair` takes the same lock; a plain check runs next to the daemon.

- **A sustained anomaly escalates to L3** (spec §3.4.1, "escalation
  from L2"). A crossed load, CPU or swap threshold switched on L2 and
  nothing more, however long it lasted. Now, once it has held on every
  tick for `collection.l2.escalate_after` (default 300 s; 0 turns it
  off), L3 collects at 1 Hz until the anomaly has been gone for
  `grace_period`; a dip inside that grace keeps L3. The daemon logs
  both ends ("anomaly held for 300 s (load 1m 6.10, ...): collecting
  at L3 until it clears"). The new `tests/smoke/escalate.sh` checks it
  on the daemon with a CPU threshold below zero.

- **The L2 and L1 rings keep filling while a finer level collects**
  (spec §3.4.1). While a client held L3, or a custom level ran, only
  that level's ring got records: the L1 ring, the one that keeps weeks
  of history, had a hole for every dashboard session, and once the raw
  ring wrapped nothing was left of those hours. `TierAggregator`, which
  existed but was not used, is now wired into `TierManager::store()`:
  L3 and custom-level samples are also folded into one record per L2
  interval in `tier2.ring`, and everything above L1 into one record
  per L1 interval in `tier3.ring`. Rates, load, memory and swap are
  averaged; process counts, temperature and RSS keep their maximum,
  entropy its minimum, file events their sum; counts, totals and
  cumulative fields their last value. A window ends after its
  interval, when the level drops to the ring's own (the folded record
  is written first, so every ring stays in time order), or on shutdown.
- **`"folded":true` on summary records.** The flag lives in the
  record header's spare byte (bit 0); records of older rings read as
  unfolded. `/api/range?level=L1` (or
  `L2`, `tier=2|3`) returns them flagged; `level=all` (the dashboard's
  history chart) leaves a folded record out where a finer ring still
  holds samples from its window, so a stretch is drawn once, at the
  finest level still on disk, and the summary takes over once the raw
  samples are overwritten.

### Changed

- **Folded records average each group over the samples that have it**,
  and keep every group some sample had; `budyk suggest-rules` leaves a
  sample out of a metric's statistics when it lacks that group. A
  zero from a group that was not collected no longer pulls a mean down.

- **`proc.total` counts processes on Linux too.** It came from
  `/proc/loadavg`, which counts threads; the collector now reads every
  `/proc/<pid>/stat` (for the states), and `proc.threads` has the
  thread count. FreeBSD always counted processes.

- **`budyk suggest-rules` opens `tier1.ring` read-only.** It opened it
  for writing and created it when missing: run as root before the
  daemon's first start, it left a root-owned ring the daemon could not
  open.

- **CRC32C by table.** Every record read checks a CRC32C, computed
  bit by bit until now; a 256-entry table (same polynomial, identical
  values, checked against the bitwise definition in the tests) makes it
  about eight times faster. `/api/range?level=all` over a day of L3
  went from 36 ms to 16 ms.

### Fixed

- **No CPU spike when a dashboard opens.** Its `/api/samples` poll and
  its WebSocket connect, a few ms apart, each woke the collection loop,
  and the second tick computed its rates over those few ms: a core that
  ran for 3 ms of 3 read 100%, and the spike went into the L3 ring and
  to the rules. A wake-up now cuts a sleep to no less than 250 ms after
  the tick (a level's own shorter interval still applies);
  `tests/smoke/modes.sh` checks that two wake-ups at once leave no two
  ticks closer than that.

## [0.6.5] — 2026-10-07

A maintenance release that finishes the code review. Security: the
HTTP parser is strict about the request line, the headers and
`Content-Length` (400, 431 or 501 instead of a guess), ring files are
created 0600, `suggest-rules --ai` no longer runs curl through a shell,
the `exec()` allowlist compares resolved paths, and the session cookie
is `Secure` behind a TLS proxy. Two changes rule authors may notice:
the sample tables (`cpu`, `mem`, ...) are read-only, so a rule that
assigned to them now gets an error, and a YAML rule whose `for_ticks`
or `cooldown` is not a whole number is rejected instead of half-read.
`hot_buffer.warm_grace` now takes effect, `budyk serve` creates a
missing `data_dir`, and a plain `cmake -B build` is a hardened Release
build. CI adds ThreadSanitizer, more clang-tidy checks, and the smoke
scripts on FreeBSD against the static binary. No change to the
configuration keys or the on-disk format.

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
- **apt gives up on a hung mirror.** A stalled Ubuntu mirror held
  `apt-get update` until the job's `timeout-minutes` ran out, with
  apt's own `Acquire::http::Timeout` set or not. The Ubuntu jobs now
  install through `.github/scripts/apt-install.sh`, which runs
  `apt-get update` (2 min) and the package downloads (4 min) under
  `timeout(1)` with three attempts each; a repeated download keeps the
  packages already fetched. Unpacking is not timed, since stopping
  dpkg half-way breaks every later apt call.
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

[0.8.2]: https://github.com/click0/budyk/releases/tag/v0.8.2
[0.8.1]: https://github.com/click0/budyk/releases/tag/v0.8.1
[0.8.0]: https://github.com/click0/budyk/releases/tag/v0.8.0
[0.7.0]: https://github.com/click0/budyk/releases/tag/v0.7.0
[0.6.5]: https://github.com/click0/budyk/releases/tag/v0.6.5
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
