# CLAUDE.md — project context for Claude Code

## What is this project?

budyk (Ukrainian "будик" — alarm clock) is a lightweight, self-contained server
monitoring daemon for FreeBSD. Single static binary, no external dependencies,
no database. BSD-3-Clause license.

## Architecture overview

- **Language:** C++17 (business logic) + C (platform collectors)
- **Build:** CMake 3.22+
- **Key innovation:** 3-level adaptive collection model (L1 heartbeat / L2 watchful / L3 active)
- **Rule engine:** embedded Lua 5.4 with sandboxed `watch()` API
- **Storage:** tiered ring-buffer files (mmap, CRC32C, level markers)
- **Web:** embedded HTTP/WS server (mongoose or libwebsockets — TBD)
- **TUI:** ncurses-based terminal UI

## Directory structure

```
src/core/          — Sample struct, MetricSource interface, codec (C++, no I/O)
src/collector/     — platform-specific metric collection (C)
  freebsd/         — sysctl, devstat, kvm, getifaddrs
  linux/           — /proc, /sys parsers
src/scheduler/     — L1↔L2↔L3 tick scheduler with anomaly detection
src/hot_buffer/    — in-memory circular buffer for WS catch-up (RAM-only)
src/storage/       — tiered ring-buffer engine (mmap, pwrite, atomic write_idx)
src/rules/         — Lua 5.4 rule engine (sandbox, watch/alert/exec/escalate)
src/ai/            — AI-assisted rule generation (Tier A: local stats, Tier B: LLM)
src/web/           — HTTP + WebSocket server + embedded SPA
src/tui/           — terminal UI
src/config/        — YAML config loader
```

## Build commands

```sh
cmake -B build -DBUDYK_PLATFORM=freebsd   # or linux
cmake --build build -j$(nproc)
ctest --test-dir build                     # run tests
```

## Coding conventions

- SPDX license header in every file
- C++17, no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`)
- Collectors are plain C (no C++ in platform-specific code)
- Errors via return codes, not exceptions
- No dynamic allocation in hot paths (collector tick, rule eval)
- All I/O non-blocking via event loop
- `#ifdef BUDYK_FREEBSD` / `#ifdef BUDYK_LINUX` for platform branching

## Key design decisions

1. Collector runs in a separate thread; results piped to event loop
2. Lua rules sandboxed: no io/os/loadfile/require; exec() gated by --enable-exec
3. Hot buffer is RAM-only, never touches disk
4. Storage records carry absolute timestamps + level markers (L1/L2/L3)
5. Ring-buffer write_idx updated atomically via mmap — crash loses at most 1 record

## Full technical specification

See `docs/budyk-spec-en.md` (English) and `docs/budyk-spec-uk.md` (Ukrainian).
