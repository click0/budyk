# Changelog

All notable changes to budyk will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
