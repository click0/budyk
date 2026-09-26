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
| **L2 Watchful** | 15–60 s | a threshold (load, CPU, swap) is crossed |
| **L3 Active** | 1 Hz | a dashboard or TUI client is connected |

It escalates the moment an anomaly shows up or someone opens the
dashboard, and steps back down after a hysteresis period. On a quiet
host budyk mostly sleeps.

## Features

- **Metrics:** CPU, memory, swap, load average, disk I/O, network,
  processes, entropy, temperature, uptime, and budyk's own CPU/RSS.
- **Tiered storage:** raw 1 Hz samples plus 1-minute and 5-minute
  aggregates in size-capped ring files, with no database.
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
    for_ticks = 5,       -- consecutive ticks the condition must hold (default 1)
    cooldown  = 60,      -- ticks to stay quiet after firing (default = for_ticks)
    action    = function()
        alert("high_cpu", "warning",
              string.format("CPU at %.0f%%", cpu.total_percent))
    end,
})
```

`when` is required. `action` has to be a **function**: a rule without
one counts its firings but does nothing.

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
| `GET` | `/api/range` | yes | stored history: `since`, `until` (nanoseconds since the epoch), `tier` (`1` raw, `2` 1-min, `3` 5-min), `limit` (up to 5000) |
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
