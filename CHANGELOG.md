# Changelog

All notable changes to budyk will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
