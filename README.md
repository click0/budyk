# budyk

**English** | [Українська](README.uk.md)

**Lightweight server monitoring with adaptive collection.**

budyk (Ukrainian "будик", alarm clock) is a self-contained monitoring
daemon for FreeBSD and Linux servers. It ships as one static binary with
no runtime dependencies and no database: metrics live in fixed-size ring
files on disk, the dashboard is embedded in the binary, and alerting rules
are plain Lua or YAML.

## Key idea: collect only as much as someone needs

Most monitoring tools sample every second whether or not anyone is
looking. budyk switches between three collection levels:

| Level | Cadence | When |
|-------|---------|------|
| **L1 Heartbeat** | every 5 min | nobody is watching and the system is healthy |
| **L2 Watchful** | 30 s | a threshold (load, CPU, swap) is crossed |
| **L3 Active** | 1 Hz | a dashboard or TUI client is connected |

Opening the dashboard switches to L3 at once. A crossed threshold is
seen at the next tick and switches to L2. Each level steps back down
after a hold period: 60 s after the last client leaves, 5 min after the
threshold clears. All intervals and thresholds are set in the config,
and you can add your own levels (see [Custom levels](#custom-levels)).
On a quiet host budyk mostly sleeps.

## How it works

budyk has no central server. Each server runs its own `budyk serve`,
and that one process collects the metrics, stores them, evaluates the
rules and serves its own dashboard. A browser or `budyk tui` connects
straight to it.

```
budyk serve (one process per server)
│
│  collection loop, one pass per tick; the level sets the gap (defaults):
│  L1 5 min · L2 30 s · L3 1 s · custom levels: your own
│
│  collectors ─► sample ─► scheduler picks the level ─┬─► ring file for that level ─► GET /api/range
│  (sysctl,                                           ├─► hot buffer (RAM, 300)    ─► GET /api/samples
│   /proc)                                            ├─► rule engine              ─► alert channels
│                                                     └─► WebSocket push           ─► open dashboards
│
└─ HTTP server (own thread, :8080): dashboard page, REST API, /api/ws
     a new WebSocket client or TUI poll wakes the loop ─► L3 immediately
```

On each tick the collection loop:

1. reads the kernel counters (`sysctl`, devstat and kvm on FreeBSD,
   `/proc` and `/sys` on Linux) into one sample;
2. asks the scheduler for the level: **L3** while a dashboard or TUI is
   connected and for `grace_period` (60 s) after it leaves, **L2** while
   a load, CPU or swap threshold is crossed and for `hysteresis` (5 min)
   after, **L1** otherwise, or a [custom level](#custom-levels) whose
   priority is higher;
3. appends the sample to the ring file for that level (tier 1 for L3,
   2 for L2, 3 for L1, `level-<name>.ring` for a custom level), each
   record tagged with its level;
4. keeps the last 300 samples in an in-memory hot buffer;
5. evaluates the rules and sends any alerts;
6. pushes the sample to every open WebSocket;
7. sleeps until the next tick.

**How the dashboard gets its data.** The daemon serves the dashboard
page itself. The page:

- calls `/api/samples` first to check the session, and shows the login
  form if the answer is 401;
- opens a WebSocket at `/api/ws`. The first frame is the hot-buffer
  history, then one frame arrives per tick;
- loads the history chart from `/api/range?level=all` (every level's
  ring, merged and thinned to fit) and refreshes it every minute.

Opening the WebSocket wakes the loop, so the level moves to L3 and
samples arrive every second from the first frame on. When the last
dashboard closes, the daemon stays at L3 for the grace period and then
steps back down. `budyk tui` has no WebSocket: it polls `/api/samples`
every second, and a poll counts as a connected client.

**Several servers.** Run budyk on each one and open each server's own
dashboard, for example through an SSH tunnel or a reverse proxy. A
single dashboard that combines servers isn't part of budyk yet.

## Features

- **Metrics:** CPU, memory, swap, load average, disk I/O, network,
  processes, entropy, temperature, uptime, and budyk's own CPU/RSS.
- **Ring-file storage:** one size-capped file per level, so 1 Hz detail
  from busy or watched periods sits next to sparse 5-minute heartbeats.
  No database.
- **Web dashboard:** live charts over WebSocket, plus a history view
  over the stored tiers. Optional password login (Argon2id).
- **Terminal UI:** `budyk tui`.
- **Custom levels:** your own cadences next to L1–L3, such as 2 Hz
  under pressure or 30 min on an idle box, switched on by a condition or
  by a rule.
- **Rules:** Lua `watch()` rules or a simple YAML form, reloaded on
  `SIGHUP` without a restart. Cooldowns survive restarts.
- **Alert channels:** ntfy, Discord, Telegram, SMTP e-mail, Twilio SMS.
- **Incident response:** a file-change watcher (`/etc/sudoers`,
  `/etc/passwd`, …) and `freeze()` / `unfreeze()` to pause a runaway
  process. Both are off by default.
- **Rule suggestions:** `budyk suggest-rules` proposes thresholds from
  collected history.

## Install

### Release binaries

Each [release](https://github.com/click0/budyk/releases) has static
binaries for Linux amd64 and FreeBSD 14.2 / 15.0 amd64:

```
budyk-<version>-linux-amd64.tar.gz
budyk-<version>-freebsd14.2-amd64.tar.gz
budyk-<version>-freebsd15.0-amd64.tar.gz
```

Each archive also has a `-debug` variant with symbols, and a `.sha256`
checksum file. Unpack it and put `budyk` somewhere in your `PATH`.

### Build from source

Dependencies:

```sh
# FreeBSD (ncurses is in base)
pkg install cmake pkgconf lua54 libyaml libargon2

# Debian / Ubuntu
apt install cmake g++ pkg-config liblua5.4-dev libyaml-dev libargon2-dev libncurses-dev
```

Build, test and install:

```sh
cmake -B build
cmake --build build -j
ctest --test-dir build
cmake --install build      # binary, example config, rules, man page
```

The build is fully static by default. Pass `-DSTATIC_LINK=OFF` to link
against shared libraries instead.

### Running as a service

- **FreeBSD:** a port skeleton and an rc.d script are in
  [`addons/freebsd/`](addons/freebsd/). The rc.d script reads
  `budyk_enable`, `budyk_config`, `budyk_user` and `budyk_flags`.
- **Linux:** a hardened systemd unit is in
  [`addons/linux/budyk.service`](addons/linux/budyk.service).
- **Docker:** a `Dockerfile` and `docker-compose.yml` are in
  [`addons/docker/`](addons/docker/).

## Quick start

```sh
cp config.example.yaml config.yaml     # edit data_dir, listen, port
budyk serve --config config.yaml
```

Open the dashboard at <http://127.0.0.1:8080>. For the terminal UI:

```sh
budyk tui [--host 127.0.0.1] [--port 8080]
```

To protect the dashboard with a password, generate a hash and put it in
the config:

```sh
budyk hash-password                    # prints an $argon2id$... hash
```

```yaml
web:
  auth:
    enabled: true
    password_hash: "$argon2id$v=19$..."   # keep the quotes
```

If the dashboard is password-protected, `budyk tui` asks for the password
once at startup (without echoing it) and logs in. If the session later
expires, for example after a daemon restart, it logs in again by itself.

All settings are documented in
[`config.example.yaml`](config.example.yaml).

## Custom levels

Besides L1, L2 and L3 you can define up to 16 levels of your own under
`collection.levels`:

```yaml
collection:
  levels:
    - name: burst          # 2 Hz while the box is under real pressure
      interval: 0.5        # seconds between ticks; fractions allowed
      priority: 40         # above L3, so it beats an open dashboard too
      when: "cpu.total_percent > 95 or load.avg_1m > cpu.count * 2"
      hold: 120            # stay 2 min after the condition clears
      storage_mb: 100      # its own ring file, level-burst.ring
    - name: deep_sleep     # slower than L1 on a truly idle box
      interval: 1800
      priority: 5          # above L1 only: any anomaly or client wins
      when: "load.avg_1m < 0.05 and cpu.total_percent < 2"
```

**Which level runs.** Every level is either active or not, and the
active level with the highest `priority` wins. On a tie, the shorter
interval wins. The built-ins have fixed priorities:

| Level | Priority | Active when |
|-------|----------|-------------|
| L1 | 0 | always (the fallback) |
| L2 | 20 | a threshold is crossed, and for `hysteresis` after |
| L3 | 30 | a dashboard or TUI is connected, and for `grace_period` after |
| custom | yours | its `when` is true (and for `hold` seconds after), or a rule called `escalate()` |

So `burst` (40) wins even while someone watches the dashboard, and
`deep_sleep` (5) only runs when nothing else is active.

**Switching a level on from a rule.** Leave `when` out and call
`escalate()` from any rule:

```lua
watch("disk_storm", {
    when   = function() return disk.write_bytes_per_sec > 500 * 1024 * 1024 end,
    action = function() escalate("burst", 300) end,   -- 5 minutes of 2 Hz
})
```

**Fields.** `name`, `interval` and `priority` are required. `name` uses
letters, digits, `_` or `-`, can't be L1–L3, and must be unique.
`interval` is 0.1–86400 s. `when`, `hold` (default 0) and `storage_mb`
(default 50) are optional. An invalid level is skipped with a message
in the daemon log, and the other levels still load. A `when` that
doesn't compile is also logged; that level can then only be switched on
with `escalate()`.

**Where the data goes.** Samples taken at a custom level are stored in
that level's own ring file and carry its id in their `level` field.
`/api/levels` maps ids to names, and `/api/range?level=<name>` reads one
level. The dashboard shows the current level and includes every level
in its history chart.

## Rules

Point `rules.path` at a `.lua` or `.yaml` file. After editing it, send
`SIGHUP` (`kill -HUP <pid>`) to reload without a restart. Per-rule
cooldowns carry over the reload.

### Lua

```lua
watch("high_cpu", {
    when      = function() return cpu.total_percent > 90 end,
    for_ticks = 5,           -- consecutive ticks the condition must hold (default 1)
    cooldown  = 60,          -- ticks to stay quiet after firing (default 0)
    severity  = "warning",   -- "info" | "warning" (default) | "critical"
    message   = "CPU above 90%",   -- default: the rule name
})

-- A function action, for anything beyond a fixed message:
watch("swap_under_load", {
    when   = function() return swap.used_percent > 80 and load.avg_1m > cpu.count end,
    action = function()
        alert("swap_under_load", "critical",
              string.format("swap %.0f%% used, load %.2f", swap.used_percent, load.avg_1m))
    end,
})
```

`when` is required. `action` is one of:

| `action` | On fire |
|----------|---------|
| `"alert"` (default) | Sends `message` with `severity` to every alert channel. |
| `"log"` | Writes `[budyk] <message>` to the daemon log. |
| a function | Calls it with no arguments. `severity` and `message` aren't used. |

Anything else, including a table of actions, is an error: loading stops
at that rule, and the daemon log shows the file, line and rule name.
Rules defined above it stay active.
[`rules/examples.lua`](rules/examples.lua) has more examples.

Functions available to rules:

| Function | Purpose |
|----------|---------|
| `alert(name, severity, message)` | Send to every configured channel. `severity` is `"info"`, `"warning"` (default) or `"critical"`. |
| `print(...)` | Write to the daemon log. |
| `exec(cmd [, timeout_s])` | Run a program: a string or an argv table, absolute path, 30 s timeout by default. **Off by default.** Enable with `rules.exec.enabled` or `--enable-exec`, and restrict it with `rules.exec.allow`. The command runs in the background: the call returns `{ queued = true }` at once (or `{ queued = false, error = "exec queue full" }`), the rules carry on, and the outcome is logged. To run it inline and get its result, `exec(cmd, { timeout = 5, wait = true })` returns `{ ok, exit_status, signal, timed_out, elapsed_seconds }`; the tick waits, so that timeout is capped at 60 s. |
| `freeze(pid)` / `unfreeze(pid)` | Send `SIGSTOP` / `SIGCONT`. **Off by default.** Enable with `rules.freeze.enabled` or `--enable-freeze`, and restrict by process name with `rules.freeze.allow`. |
| `escalate(level [, seconds])` | Keep a collection level active for `seconds` (default 60): a [custom level](#custom-levels)'s name, `"L2"` or `"L3"`. Takes effect for the current sleep. Raises for an unknown level. |

Metrics are refreshed every tick as globals:

| Global | Fields |
|--------|--------|
| `cpu` | `total_percent`, `count` |
| `mem` | `total`, `available`, `available_percent` |
| `swap` | `total`, `used`, `used_percent` |
| `load` | `avg_1m`, `avg_5m`, `avg_15m` |
| `disk` | `read_bytes_per_sec`, `write_bytes_per_sec`, `device_count` |
| `net` | `rx_bytes_per_sec`, `tx_bytes_per_sec`, `interface_count` |
| `proc` | `total`, `running` |
| `entropy` | `available_bits`, `present` |
| `thermal` | `max_celsius`, `sensor_count`, `present` |
| `self_` | `rss_bytes`, `peak_rss_bytes`, `cpu_user_seconds`, `cpu_system_seconds` |
| `uptime_seconds` | number |
| `files` | `files["/path"].modifies`, `.deletes`, `.tampered` (with `security.file_watch` on) |

Rules run in a sandbox that has only the base library, `math`, `string`
and `table`. `io`, `os`, `require`, `load`, `loadfile` and `dofile` are
not available.

A rule cannot stall the daemon. Each call into Lua (a `when`, an
action, a custom level's `when`, the rules file being loaded) may run
at most `rules.limits.instructions` VM instructions (default 1 000 000,
about 10 ms), and the engine may hold at most `rules.limits.memory_mb`
(default 16). A rule that overruns gets an error logged once and is
skipped on that tick; the other rules and collection carry on.

### YAML

For simple threshold rules:

```yaml
- name: disk_busy
  when: "disk.write_bytes_per_sec > 200000000"   # Lua expression
  for_ticks: 3
  cooldown: 60
  severity: critical        # info | warning | critical
  action: alert             # alert | log
  message: "Disk writes above 200 MB/s"
```

Each entry is converted to a `watch()` call when the file is loaded.

### Suggested rules

```sh
budyk suggest-rules --config config.yaml --window 7d --output suggested.lua
```

This reads the collected history and writes `watch()` rules with
thresholds derived from it. Review them before use.

## Alerts

Configure channels under `alerts.channels`. `alert()` sends to every
channel; if one fails, the rest still get the message. Sending happens
on a separate thread: a channel that is slow or down never delays
collection. The queue holds 256 alerts; beyond that, new ones are
dropped and logged until it drains.

| `type` | Destination |
|--------|-------------|
| `ntfy` | ntfy.sh or a self-hosted ntfy server |
| `discord` | Discord webhook |
| `telegram` | Telegram Bot API (chat, group or channel) |
| `smtp` | E-mail through an SMTP / SMTPS relay |
| `twilio` | SMS through Twilio |

See [`config.example.yaml`](config.example.yaml) for the fields each type
needs.

## HTTP API

| Method | Path | Auth | Returns |
|--------|------|------|---------|
| `GET` | `/api/health` | no | status, version |
| `POST` | `/api/auth/login` | no | body `{"password": "..."}`; sets the `budyk_session` cookie |
| `POST` | `/api/auth/logout` | no | clears the session |
| `GET` | `/api/samples` | yes | recent samples from the in-memory buffer |
| `GET` | `/api/range` | yes | stored history: `since`, `until` (nanoseconds since the epoch), `limit` (up to 5000), and either `level` (`L1`–`L3`, a custom level's name, or `all` for every level merged and thinned over the window) or `tier` (`1` = L3 samples, `2` = L2, `3` = L1) |
| `GET` | `/api/levels` | yes | every level: `id` (the value of a sample's `level`), `name`, `interval_ms`, `priority`, `builtin` |
| `GET` | `/api/ws` | yes | WebSocket live stream |

"Auth" applies only when `web.auth.enabled` is `true`.

## Signals

| Signal | Effect |
|--------|--------|
| `SIGHUP` | Reload the rules file. |
| `SIGTERM`, `SIGINT` | Save rule state and shut down cleanly. |

## Platforms

- **FreeBSD 14.2, 15.0:** primary target, built and tested in CI.
- **Linux:** built and tested in CI on Ubuntu.

Other versions and systems aren't tested. NetBSD and OpenBSD have no
collector yet.

## License

[BSD-3-Clause](LICENSE)
