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
threshold clears. All intervals and thresholds are set in the config.
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
│  L1 5 min · L2 30 s · L3 1 s
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
   after, **L1** otherwise;
3. appends the sample to the ring file for that level (tier 1 for L3,
   2 for L2, 3 for L1), each record tagged with its level;
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
- loads the history chart from `/api/range` and refreshes it every
  minute.

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

> The TUI calls the HTTP API without logging in, so it only works while
> `web.auth.enabled` is `false`.

All settings are documented in
[`config.example.yaml`](config.example.yaml).

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
| `exec(cmd [, timeout_s])` | Run a program: a string or an argv table, absolute path, 30 s timeout by default. **Off by default.** Enable with `rules.exec.enabled` or `--enable-exec`, and restrict it with `rules.exec.allow`. Returns `{ ok, exit_status, signal, timed_out, elapsed_seconds }`. |
| `freeze(pid)` / `unfreeze(pid)` | Send `SIGSTOP` / `SIGCONT`. **Off by default.** Enable with `rules.freeze.enabled` or `--enable-freeze`, and restrict by process name with `rules.freeze.allow`. |
| `escalate()` | Reserved. It currently does nothing. |

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
channel; if one fails, the rest still get the message.

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
| `GET` | `/api/health` | no | status, version, data directory |
| `POST` | `/api/auth/login` | no | body `{"password": "..."}`; sets the `budyk_session` cookie |
| `POST` | `/api/auth/logout` | no | clears the session |
| `GET` | `/api/samples` | yes | recent samples from the in-memory buffer |
| `GET` | `/api/range` | yes | stored history: `since`, `until` (nanoseconds since the epoch), `tier` (`1` = L3 samples, `2` = L2, `3` = L1), `limit` (up to 5000) |
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
