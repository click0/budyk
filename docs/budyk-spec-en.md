# Technical Specification: budyk — Lightweight FreeBSD Server Monitoring with Adaptive Collection

> **Project name:** budyk (Ukrainian "будик" — alarm clock)
> **License:** BSD-3-Clause
> **Target platform:** FreeBSD 13.x / 14.x / 15.x (amd64, arm64); nice-to-have — NetBSD/OpenBSD/Linux compatibility
> **Language:** C++ (C++17 minimum, C++20 preferred) + C for platform-specific collectors
> **Build system:** CMake 3.22+ (or Meson, to be decided at M0)
> **Form factor:** single repository with modular build + statically linked CLI binary
> **Reference systems:** kula, Netdata, btop, Glances, sysstat/sar, atop, RRDtool

---

## 0. Positioning

### 0.1. What We Are Building
An original project (BSD-3-Clause) — a self-contained monitoring daemon for FreeBSD servers. Single binary, no external dependencies, no database. The key differentiator from existing tools is an **adaptive collection model** (§3.4): the system truly sleeps when not needed and instantly wakes up when it is.

### 0.2. What This Is Not
This is not a port, fork, or derivative work of any specific project. Ideas and approaches are borrowed from multiple systems (see Appendix A); the implementation is entirely from scratch.

### 0.3. Reference Systems
Each solves part of the problem; none solves all of it:

| System | What we take | What we skip |
|--------|-------------|-------------|
| **kula** (Go, AGPL-3.0) | Ring-buffer tier architecture, embedded SPA, single-binary design, `Sample` struct as the contract between collector and storage | Always-on 1 Hz (wasteful), JSON codec (slow), `/proc` coupling |
| **Netdata** (C, GPL-3.0) | Tier storage architecture, ML anomaly detection (concept), parent-child streaming | Heavy footprint (~200 MB RSS), cloud dependency, always-on |
| **btop / htop** (C++/C) | On-demand model (collect only while running), minimal footprint | No history, no web UI, no daemon |
| **Glances** (Python) | REST API + WebSocket stream, cross-platform, psutil abstraction | Python overhead, no on-disk persistence |
| **sysstat/sar** (C) | Binary metric storage format, cron-based heartbeat, post-mortem replay | Fixed interval, no web UI, no real-time |
| **atop** (C) | Post-mortem replay of binary logs, full system snapshot | Heavy snapshots, fixed interval |
| **RRDtool** (C) | Ring-buffer tier pioneer, predictable disk usage, consolidation functions | Outdated API, separate daemon |

---

## 1. Phase 1 — Reference System Study

### 1.1. What to Extract from Each Reference System
For kula (closest in scope) — deep per-module analysis. For the rest — targeted study of specific subsystems.

**From kula** (`github.com/c0m4r/kula`, Go, AGPL-3.0):

- **`internal/collector/`**
  - List of all `/proc/*` and `/sys/*` paths read (grep for `os.Open`, `os.ReadFile`).
  - Format of each input file (columns, units, deltas vs absolute values).
  - Aggregation algorithm: how per-core deltas are computed, how disk `%util` is calculated, how TCP counters are aggregated.
  - Sampling frequency, jitter compensation, gap handling.
  - `Sample` struct definition (`collector/types.go`) — the contract between collectors and storage.

- **`internal/storage/`**
  - Exact on-disk ring-buffer file format: header, record, tail, endianness.
  - Codec (`codec.go`) — JSON or binary? If JSON — it is a CPU bottleneck at 1 Hz; our project should use `msgpack` or a custom binary format.
  - Rotation logic: what is written to the header, how the write-cursor is tracked, how crash-recovery works.
  - Aggregation algorithm Tier 1 → Tier 2 → Tier 3: which fields are averaged, which use last-value, which use max/min.
  - Default sizes: 250 / 150 / 50 MB → estimate how many records this holds and how many hours/days of history.

- **`internal/web/`**
  - Complete REST endpoint registry (method, path, query/body, response format).
  - WebSocket protocol: frame format, heartbeat, backpressure, broadcast hub.
  - Auth flow: cookie issuance, sliding expiration, at-rest session persistence, Argon2id parameters (m, t, p).
  - SPA embedding: `//go:embed static/*` → C/C++ equivalent via `xxd -i` or `incbin`.

- **`internal/tui/`**
  - Screens, navigation, which metrics are displayed.
  - TUI refresh rate and how it subscribes to the collector.

- **`cmd/kula/main.go`**
  - Subcommands: `serve`, `tui`, `hash-password`, ... — collect the full list and describe each as a CLI contract.

- **`config.example.yaml`**
  - Full config schema with types, defaults, env-override rules (`KULA_*`).

**From Netdata** (targeted):
- `dbengine` tier architecture — how 3-tier storage is organized, how aggregation works, which consolidation functions.
- ML anomaly detection — which algorithms (k-means at edge), which features, overhead.

**From sysstat/sar** (targeted):
- Binary `sa*` file format — how metrics are encoded, headers, endianness.
- Which sysctl/procfs values `sadc` collects and how it aggregates.

**From atop** (targeted):
- Binary log format, replay mechanism, how "time travel" across snapshots works.

### 1.2. Phase 1 Deliverable
`docs/reference-study/` — a set of markdown files, one per system, plus:
- `data-contract.md` — Sample struct and its evolution through tiers
- `storage-format.md` — byte-level ring-buffer format
- `http-api.md` — OpenAPI spec (YAML)
- `ws-protocol.md` — frame description
- `config-schema.md` — JSON Schema for config

---

## 2. Phase 2 — Linux → FreeBSD Data Source Mapping

This is the critical phase. FreeBSD has **no** `/proc` by default and will **never** have an equivalent `/sys`. We need an abstraction — an abstract class `MetricSource` (C++) or vtable interface (C) — behind which Linux and FreeBSD implementations hide.

### 2.1. Mapping Table (minimum)

| Metric | Linux (source) | FreeBSD (source) | C/C++ API |
|--------|----------------|-------------------|-----------|
| CPU per-core (user/sys/idle/iowait/irq/steal) | `/proc/stat` | `sysctl kern.cp_times` (array `long[CPUSTATES * ncpu]`) | `sysctlbyname(3)` |
| Load average | `/proc/loadavg` | `sysctl vm.loadavg` or `getloadavg(3)` | `getloadavg(3)` — POSIX |
| Memory | `/proc/meminfo` | `sysctl vm.stats.vm.*`, `hw.physmem`, `hw.pagesize` | `sysctlbyname(3)` |
| Swap | `/proc/swaps` + `/proc/meminfo` | `kvm_getswapinfo(3)` or `sysctl vm.swap_info` | `<kvm.h>` + `-lkvm` |
| Uptime | `/proc/uptime` | `sysctl kern.boottime` + `CLOCK_MONOTONIC` | `clock_gettime(2)` — POSIX |
| Hostname | `/proc/sys/kernel/hostname` | `gethostname(3)` | `gethostname(3)` — POSIX |
| Entropy | `/proc/sys/kernel/random/entropy_avail` | Not directly exposed on FreeBSD — either omit or use `kern.random.*` (different semantics) | `sysctlbyname(3)` |
| Clock sync | `/proc/sys/kernel/...`, NTP-check | `ntp_gettime(2)` / `ntp_adjtime(2)` (`struct ntptimeval`) | `<sys/timex.h>` |
| Network per-interface | `/proc/net/dev` | `getifaddrs(3)` + `struct if_data` (via `AF_LINK` address) | `getifaddrs(3)`, `<net/if.h>` |
| TCP/UDP/ICMP counters | `/proc/net/snmp`, `netstat -s` | `sysctl net.inet.{tcp,udp,ip,icmp}.stats` (returns `struct *stat`) | `sysctlbyname(3)` + `<netinet/tcp_var.h>` etc. |
| Sockets (established/...) | `/proc/net/tcp[6]` | `sysctl net.inet.tcp.pcblist` + `xtcpcb` | `sysctlbyname(3)` + `<netinet/tcp_var.h>` |
| Disk I/O per-device | `/proc/diskstats` | `devstat(3)` API (`devstat_getdevs`, `devstat_compute_statistics`) | `<devstat.h>` + `-ldevstat` |
| Filesystem usage | `/proc/mounts` + `statfs` | `getmntinfo(3)` + `struct statfs` | `getmntinfo(3)` — BSD |
| Inode usage | `statfs.f_files/f_ffree` | `statfs.f_files/f_ffree` (identical) | ditto |
| Processes by state | `/proc/*/stat` scan | `sysctl kern.proc.all` → array of `struct kinfo_proc`, field `ki_stat` | `sysctlbyname(3)` + `<sys/user.h>` |
| Threads total | `/proc/*/status` | `kinfo_proc.ki_numthreads` | ditto |
| Self metrics (CPU%, RSS) | `/proc/self/stat`, `/proc/self/status` | `kinfo_getproc(getpid())` from `libutil` | `<libutil.h>` + `-lutil` |
| Temperature/thermal | `/sys/class/thermal/*` | `sysctl dev.cpu.N.temperature`, `hw.acpi.thermal.*` | `sysctlbyname(3)` |
| GPU NVIDIA | nvidia-smi/NVML | NVML on FreeBSD via driver — FreeBSD impl is a stub or via `nvidia-smi` exec | NVML SDK headers / `popen("nvidia-smi ...")` |

### 2.2. Phase 2 Deliverable
`docs/platform-mapping.md` + prototype module `collector_freebsd.c` with stubs for all functions returning `ENOSYS`. This allows upper layers to be developed in parallel.

---

## 3. Phase 3 — Architecture (C/C++)

### 3.1. Project Layout

```
budyk/
├── CMakeLists.txt                  # top-level, includes all modules
├── cmake/
│   ├── platform.cmake              # detect OS, set platform-specific flags
│   └── embed_resources.cmake       # embed SPA into binary (xxd / incbin)
├── src/
│   ├── core/                       # Sample, interfaces, codec (C++17)
│   │   ├── sample.h                # struct Sample { CpuStats, MemStats, ... }
│   │   ├── metric_source.h         # abstract class MetricSource
│   │   ├── collection_level.h      # enum class Level { L1, L2, L3 }
│   │   └── codec.h / codec.cpp     # binary codec (msgpack or custom)
│   ├── collector/
│   │   ├── collector.h             # common interface
│   │   ├── linux/                  # implementation via /proc, /sys (C)
│   │   │   ├── cpu.c
│   │   │   ├── memory.c
│   │   │   ├── network.c
│   │   │   ├── disk.c
│   │   │   └── system.c
│   │   └── freebsd/                # implementation via sysctl, devstat, kvm (C)
│   │       ├── cpu.c
│   │       ├── memory.c
│   │       ├── network.c
│   │       ├── disk.c
│   │       └── system.c
│   ├── scheduler/                  # tick scheduler: L1↔L2↔L3, anomaly-detector (C++)
│   ├── hot_buffer/                 # lock-free circular buffer for WS catch-up (C++)
│   ├── storage/                    # tiered ring-buffer, mmap, level markers (C++)
│   │   ├── ring_file.h / .cpp      # single ring-buffer file
│   │   ├── tier_manager.h / .cpp   # multi-tier coordinator + aggregation
│   │   └── codec.h / .cpp          # on-disk binary format
│   ├── web/                        # HTTP/WebSocket server (C++)
│   │   ├── server.h / .cpp         # routes, API handlers
│   │   ├── ws_hub.h / .cpp         # broadcast hub, client management
│   │   ├── auth.h / .cpp           # Argon2id + sessions
│   │   └── static/                 # embedded SPA (html/js/css)
│   ├── tui/                        # terminal UI (C++)
│   ├── rules/                      # Lua-based rule engine (C++ + Lua C API)
│   │   ├── lua_engine.h / .cpp     # Lua VM lifecycle, sandbox setup, per-tick eval
│   │   ├── lua_bindings.h / .cpp   # expose Sample fields as read-only Lua tables
│   │   ├── lua_stdlib.h / .cpp     # watch(), alert(), exec(), escalate() builtins
│   │   └── yaml_compat.h / .cpp    # optional YAML-to-Lua transpiler for simple rules
│   ├── ai/                         # AI-assisted rule generation (C++)
│   │   ├── baseline.h / .cpp       # statistical baseline from history
│   │   ├── suggest.h / .cpp        # rule suggestion engine (local heuristics)
│   │   └── llm_client.h / .cpp     # optional: LLM API client for advanced suggestions
│   ├── config/                     # YAML config loader (C++)
│   └── main.cpp                    # CLI entry point (serve, tui, hash-password, suggest-rules)
├── third_party/                    # vendored or submodule dependencies
├── rules/                          # default rule library
│   ├── examples.lua                # example rules with comments
│   └── freebsd-defaults.lua        # FreeBSD-specific sensible defaults
├── tests/                          # unit + integration tests
├── addons/
│   ├── freebsd/                    # rc.d script, pkg-plist, Makefile for ports
│   ├── linux/                      # systemd unit, deb rules
│   └── docker/
├── docs/
│   └── budyk.8                     # man page
└── config.example.yaml
```

### 3.2. Key Dependencies

| Subsystem | Library | Rationale |
|-----------|---------|-----------|
| Event loop | **libuv** (or raw `kqueue`/`epoll`) | Cross-platform async I/O, actively used on FreeBSD. Alternative: `libevent`. |
| HTTP + WebSocket | **mongoose** (embedded, MIT) or **libwebsockets** | mongoose — single-header, ideal for embedded server. libwebsockets — heavier but more powerful. |
| YAML config | **libyaml** (C) or **yaml-cpp** (C++) | libyaml — more minimal; yaml-cpp — more convenient from C++ |
| JSON (API responses) | **cJSON** (C, MIT) or **nlohmann/json** (C++, MIT) | cJSON for minimalism; nlohmann for convenience |
| Binary codec (storage) | **msgpack-c** or **custom** | msgpack — standardized, fast. Custom — if full record size control is needed |
| Argon2 | **libargon2** (reference impl, CC0/Apache-2.0) | de facto standard |
| Rule engine | **Lua 5.4** (MIT) | ~200 KB, embeddable, FreeBSD uses Lua in bootloader since 12.0. Replaces custom DSL parser |
| TUI | **ncurses** (BSD base) or **notcurses** (modern) | ncurses ships in FreeBSD base. notcurses — nicer but adds a dependency |
| CLI parsing | **getopt_long** (base) or **argtable3** (vendored, BSD) | getopt_long — zero-dependency, in base |
| Logging | **syslog(3)** + custom structured logger | In FreeBSD base, zero-dependency |
| Hashing (CRC32C) | **SSE4.2 intrinsics** or `<zlib.h>` crc32 | For ring-buffer records |
| SPA embedding | **xxd** or **incbin** (compile-time embed) | Equivalent of `go:embed` |
| Testing | **cmocka** (C, Apache-2.0) or **Google Test** (C++, BSD) | cmocka — for C collector modules; gtest — for C++ |

**Principle: minimum dependencies.** FreeBSD base already contains: libc, libm, libkvm, libdevstat, libutil, ncurses, zlib, libcrypto. Ideal case — depend only on base + mongoose (vendored) + libyaml (ports) + libargon2 (ports).

### 3.3. Key Architectural Invariants

1. **Separation of concerns**: `core/` — pure structs and interfaces, no I/O, no heap allocation where possible. Collectors — plain C, compiled as `.o` files, linked conditionally via `#ifdef __FreeBSD__` / `#ifdef __linux__`.
2. **`class MetricSource`** (abstract) — the sole OS extension point. Compile-time selection via `cmake -DPLATFORM=freebsd`.
3. **Storage knows nothing about the collector** — accepts `struct Sample*` and writes bytes. Storage format — custom, binary, little-endian, versioned.
4. **WS hub — one fd-backed broadcast**: `pipe(2)` or custom lock-free SPMC ring. Slow-consumer disconnect.
5. **Config is immutable after startup** (except optional reload on `SIGHUP` — v2).
6. **All I/O is non-blocking** via event loop (libuv/kqueue). Heavy `sysctl`/`devstat` calls — in a separate thread, result piped into the event loop.
7. **No C++ exceptions.** Compiled with `-fno-exceptions -fno-rtti` for minimal footprint. Errors — via return codes / `std::expected` (C++23) or a custom `Result<T,E>`.
8. **Static linking by default** (`-static` or `-static-libstdc++`) — a single binary with no runtime dependencies.

### 3.4. Adaptive Collection Model

Most monitoring tools (kula, Netdata, Prometheus node_exporter, Telegraf, collectd) use an **always-on** model: collecting every second regardless of whether anyone is watching the dashboard. This is wasteful — per Netdata issue #238 and similar discussions, a monitoring daemon burns 2–5% CPU 99.99% of the time for nothing.

Our project implements a **3-level adaptive model + a separate hot buffer**.

#### 3.4.1. Three Collector Levels (nested matryoshka)

```
                  ┌─────────────────────────────────────────────┐
                  │  L3 — ACTIVE (1 Hz, full metric set)        │
                  │  Trigger: WS client connected OR             │
                  │           escalation from L2 (anomaly > N m) │
                  │  ┌─────────────────────────────────────────┐ │
                  │  │  L2 — WATCHFUL (15–60s, extended set)   │ │
                  │  │  Trigger: L1 detected anomaly OR         │ │
                  │  │           config: l2_always_on: true      │ │
                  │  │  ┌─────────────────────────────────────┐ │ │
                  │  │  │  L1 — HEARTBEAT (5 min, minimum)    │ │ │
                  │  │  │  Always running. Load, CPU%,         │ │ │
                  │  │  │  mem total, swap, uptime.            │ │ │
                  │  │  │  ~1 sysctl call, ~200 B to disk.    │ │ │
                  │  │  └─────────────────────────────────────┘ │ │
                  │  └─────────────────────────────────────────┘ │
                  └─────────────────────────────────────────────┘
```

**L1 — Heartbeat (always active).** One tick every 5 minutes. Minimum set: load average, total CPU%, total memory, swap, uptime. One `sysctl` call, ~200 bytes written to disk. Per day — 288 records × 200 B ≈ 56 KB (vs ~165 MB/day for always-on monitoring at Tier 1). Purpose: know the machine is alive; coarse picture over weeks.

**L2 — Watchful (conditional).** Tick every 15–60 seconds (configurable). Extended set: per-core CPU, per-interface net, per-device disk I/O, TCP counters, process counts. Activated by: (a) L1 detecting an anomaly (load/CPU/swap above threshold), (b) config `l2_always_on: true`. De-escalation — with 5-minute hysteresis after normalization.

**L3 — Active (client or alert).** 1 Hz, full set including thermal, entropy, clock sync, self-metrics. Activated by: WS client or TUI connection, or escalation from L2 (anomaly persisting > N minutes). De-escalation — grace period of 30–60 seconds after the last client disconnects.

**Nesting:** when L3 is active, L2 and L1 do not launch separate collections — the L3 tick contains all data. The aggregator periodically folds L3 samples into records for the L2 ring (every 15–60s) and L1 ring (every 5 min).

**Scheduler:** the central point — tick scheduler, governed by three inputs:
1. `atomic_int` — active WS/TUI subscriber count
2. `enum anomaly_state { NORMAL, ELEVATED, CRITICAL }` — anomaly-check result from the last L1 tick
3. Current level

Escalation — instant (next tick). De-escalation — with hysteresis (protection against oscillation on unstable WS connections or browser F5).

#### 3.4.2. Hot Buffer (separate entity, RAM-only)

The hot buffer **is not part of the tier storage pyramid**. It is an in-memory circular buffer of fixed size (300 records = 5 minutes at 1 Hz, ~600 KB RAM), existing solely to serve WebSocket streams.

Lifecycle:
- On WS client connect: client receives a dump of current hot buffer contents (catch-up — charts are not empty), then subscribes to broadcast.
- Hot buffer **never writes to disk**. Data is ephemeral.
- Hot buffer and Tier 1 ring file are fed from **one collector tick** — one collect → two consumers (storage + hot buffer).
- Grace period: after the last client leaves, hot buffer remains "warm" for 60 seconds (the next client instantly sees 5 minutes of charts). After grace — reset.

#### 3.4.3. Consequences for Storage

Data density in ring files is **non-uniform over time**: periods of 1-second resolution alternate with 5-minute gaps. This is a fundamental difference from always-on monitors (constant density).

Consequences:
1. Each record carries an **absolute timestamp** (not a delta).
2. Each record carries a **level marker** (L1/L2/L3) — the frontend knows how to interpolate and where to draw "low-resolution zones."
3. Tier 1 → Tier 2 aggregation works with irregular samples: "all records within a 1-minute window, however many there are."
4. The `/api/history` API returns data with resolution zone markers.

#### 3.4.4. Configuration

```yaml
collection:
  mode: adaptive          # always | adaptive | on-demand
  l1:
    interval: 300s        # heartbeat
    metrics: minimal      # load, cpu_total, mem_total, swap, uptime
  l2:
    interval: 30s
    always_on: false       # true = L2 always runs
    escalation_thresholds:
      load_1m: 4.0
      cpu_percent: 85
      swap_used_percent: 50
    hysteresis: 300s       # 5 min before de-escalation
  l3:
    interval: 1s
    metrics: full
    grace_period: 60s      # after last client disconnects
  hot_buffer:
    capacity: 300          # records (= 5 min at 1 Hz)
    warm_grace: 60s        # how long to keep after clients leave
```

In `always` mode — behavior identical to kula/Netdata (L3 always). In `on-demand` mode — L1 disabled, L2 disabled, collection only on client connect (hot buffer only, ring-buffer not used).

#### 3.4.5. Resource Comparison

| Scenario | always-on (kula/Netdata) | Our project (adaptive) |
|----------|--------------------------|------------------------|
| Idle, 0 clients, 24h | 86,400 records, ~165 MB disk, CPU ~2% | 288 records (L1), ~56 KB disk, CPU ≈ 0% |
| 1 client, 1 hour | 3,600 records, ~7 MB | 3,600 records (L3) + 288 (L1), ~7 MB |
| Idle, 0 clients, 30 days | ~4.8 GB Tier 1 (ring overwritten) | ~1.6 MB (L1) |

### 3.5. Storage Format (proposal)

```
[ File Header 64 B ]
  magic:       "KULA\0RB\x02"        // 8 B (v2 — with level marker support)
  version:     uint32_t              // 4 B
  tier:        uint8_t (1|2|3)       // 1 B
  _pad:        uint8_t[3]
  record_size: uint32_t              // 4 B — fixed record size
  capacity:    uint64_t              // 8 B — number of slots
  write_idx:   uint64_t              // 8 B — atomically updated
  _reserved:   uint8_t[28]

[ Record 0 ] [ Record 1 ] ... [ Record N-1 ]

Record:
  timestamp_unix_nanos: uint64_t     // absolute (not a delta!)
  level:               uint8_t      // 1=L1, 2=L2, 3=L3
  _pad:                uint8_t[1]
  crc32c:              uint32_t
  payload:             uint8_t[record_size - 14]  // msgpack-serialized Sample
```

Record written via `pwrite(2)` at a calculated offset; `write_idx` updated via `__atomic_fetch_add` in the `mmap` region. On crash, at most one record is lost.

### 3.6. Rule Engine (Lua-based)

The rule engine embeds Lua 5.4 to evaluate user-defined monitoring rules against each collected `Sample`. Instead of inventing a custom DSL with a bespoke parser, we use a real programming language — one that FreeBSD already ships in its bootloader since 12.0.

#### 3.6.1. Why Lua, Not a Custom DSL

A custom YAML-based condition language (`condition: "> 90"`, `all:`, `any:`, wildcard `*`) requires building a parser, AST evaluator, and combinator logic — roughly 2,000 lines of code that replicate what any scripting language already provides. Lua adds ~200 KB to the binary, embeds via `#include <lua.h>` with ~200 lines of glue code, and gives users full expressiveness: `and`, `or`, loops, functions, math, string operations.

Simple rules remain as readable as monit. Complex rules (computed thresholds, iteration over dynamic device sets, cross-metric correlations) are just Lua — not a limited DSL that needs extensions.

#### 3.6.2. Rule Syntax

```lua
-- rules.lua — simple example, readable without Lua experience

watch("high_cpu", {
  when = function() return cpu.total_percent > 90 end,
  for_ticks = 5,
  severity = "warning",
  action = alert
})

watch("memory_critical", {
  when = function() return mem.available_percent < 5 end,
  for_ticks = 3,
  severity = "critical",
  action = { alert, exec("/usr/local/bin/drop-caches.sh") }
})

-- Computed threshold — impossible in a simple DSL
watch("swap_under_load", {
  when = function()
    return swap.used_percent > 80 and load.avg_1m > cpu.count * 2
  end,
  for_ticks = 3,
  action = { alert, escalate }  -- force L3 collection
})

-- Iterate over interfaces — just a loop, no wildcard DSL
for name, iface in pairs(net.interfaces) do
  watch("errors_" .. name, {
    when = function() return iface.rx_errors_per_sec > 100 end,
    for_ticks = 5,
    action = alert
  })
end
```

Optionally, a thin YAML compatibility layer transpiles simple YAML rules into Lua `watch()` calls at load time — for users who prefer YAML for basic cases. Internally, the engine is always Lua.

#### 3.6.3. Sandboxing

Lua rules run in a restricted sandbox:
- No `io`, `os`, `loadfile`, `dofile`, `require` — pure logic only.
- `exec()` is a C function under our control (timeout, `setrlimit`, `--enable-exec` flag).
- `Sample` fields exposed as read-only Lua tables (`cpu`, `mem`, `net`, `disk`, `load`, `swap`).
- Execution time limited via `lua_sethook` — 10ms max per tick, script killed on timeout.
- No filesystem access, no network access, no FFI.

#### 3.6.4. Evaluation Pipeline

On each collector tick:

1. **Load** — rule files parsed once at startup (or on `SIGHUP`). Each `watch()` call registers a rule in a C-side rule array. No per-tick parsing.
2. **Bind** — current `Sample` struct fields pushed into Lua global tables (`cpu.total_percent`, `mem.available_percent`, etc.) as read-only values.
3. **Eval** — for each registered rule, call its `when()` function. Boolean result.
4. **Sustain** — increment or reset per-rule counter (C-side, not in Lua). Counter reaches `for_ticks` → fire.
5. **Act** — `alert`: log + WS event. `exec()`: fork+exec, non-blocking, with timeout. `escalate`: signal to scheduler. `log`: structured log entry.
6. **Cooldown** — after firing, rule enters cooldown (configurable, default = `for_ticks * interval`).

Performance budget: 100 rules evaluated per tick within ≤ 1ms. Lua function calls with numeric comparisons are trivially fast.

#### 3.6.5. Rules vs Collection Level

Not all metrics are available at all levels. L1 collects only `load`, `cpu_total`, `mem_total`, `swap`, `uptime`. If a rule's `when()` accesses `disk.sda.util_percent`, that field is `nil` at L1.

The engine handles this: if `when()` returns `nil` (because it accessed a nil field and Lua's comparison with nil yields nil/false), the rule is skipped for that tick, and the sustain counter is not reset. Counter survives level transitions: L3 (4/5 hits) → L1 (skipped, stays at 4) → L3 (resumes from 4).

### 3.7. AI-Assisted Rule Generation

The rule engine is useful only if it has good rules. Writing rules requires understanding normal baselines and typical failure patterns — exactly what AI is good at. This section describes an AI layer that **proposes** rules; it never autonomously applies them.

#### 3.7.1. Two Tiers of AI Assistance

**Tier A — Local Heuristics (no external API, always available).**

Built-in statistical analysis of collected history. Runs as a CLI command:

```
$ budyk suggest-rules --window 7d --output suggested-rules.lua
```

How it works:
1. Read Tier 2/Tier 3 ring-buffers for the requested window.
2. For each metric, compute: mean, stddev, p95, p99, min, max over the window.
3. Apply heuristic templates. Examples: if `cpu.total_percent` p99 is 45% → suggest threshold at 85% (≈ 2× headroom). If `memory.available_percent` ever dropped below 10% → suggest a rule at 8% with severity critical. If `disk.*.util_percent` shows sustained >90% episodes → suggest with the observed duration as `for_ticks`.
4. Output a `suggested-rules.lua` file with `watch()` calls and comments explaining rationale.

This is **not** machine learning. It is arithmetic + templates. It runs locally, does not require network access, and completes in seconds. It is always available even on air-gapped systems.

**Tier B — LLM-Assisted (optional, requires API access).**

For users who want smarter suggestions: send anonymized metric summaries (not raw data) to an LLM API and receive rule suggestions with natural-language explanations.

How it works:
1. User runs: `$ budyk suggest-rules --ai --api-key $KEY`
2. The tool computes the same statistical summary as Tier A.
3. It constructs a prompt: "Here are 7-day metric summaries for a FreeBSD server. Suggest monitoring rules as Lua `watch()` calls. For each rule, explain why."
4. Summary is anonymized: no hostnames, no IPs, no usernames. Only numeric distributions.
5. LLM responds with Lua rules + explanations. Lua is a widely known format — LLMs generate it far more reliably than a custom DSL.
6. The tool validates the response: parseable by Lua? references valid fields? thresholds within plausible range?
7. Output to `suggested-rules.lua` with `-- AI-suggested` markers.

**The user always reviews and approves.** The tool never writes directly to `config.yaml`. The workflow is always: `suggest → review → copy to config → restart/reload`.

#### 3.7.2. AI in the Dashboard (future, v1.2+)

A future version may integrate AI suggestions directly into the SPA dashboard: a "Suggest Rules" button that runs Tier A locally (via API call to the daemon) or Tier B (via browser → LLM API, with user-provided key). Results appear as a diff-style preview: "Add this rule? [Accept] [Modify] [Reject]." This is explicitly out of scope for v1.0 but the architecture should not prevent it.

#### 3.7.3. Architecture Constraints

1. **AI is never in the hot path.** Rule evaluation (§3.6) is pure arithmetic, no AI. AI only runs when the user explicitly asks for suggestions.
2. **No always-on AI.** The daemon does not phone home, does not send telemetry, does not run background ML models. Tier A is local arithmetic. Tier B is user-initiated.
3. **Tier B is optional at compile time.** `cmake -DENABLE_LLM=OFF` removes the LLM client entirely. Zero network code for LLM in the binary. Air-gapped systems get Tier A only.
4. **No API keys in config.** Keys are passed via environment variable or CLI flag, never persisted to disk by the tool.

---

## 4. Phase 4 — Implementation Plan

Milestones (each = a separate PR series, a separate `0.x` release):

| M | Scope | Definition of Done |
|---|-------|--------------------|
| M0 | Project skeleton, CMake, CI (GitHub Actions + Cirrus CI for FreeBSD 13/14/15), clang-tidy + clang-format + cppcheck | `cmake --build .` green on Linux and FreeBSD |
| M1 | `core/` — struct Sample + msgpack codec + unit tests roundtrip + `enum Level` | 100% coverage on Sample serialization |
| M2 | `collector/linux/` MVP (CPU+mem+net+disk+load) | matches `top`, `vmstat`, `/proc` data on the same host (golden tests) |
| M3 | `collector/freebsd/` MVP (CPU+mem+net+disk+load via sysctl/devstat/getifaddrs) | matches `top`, `netstat -ib`, `iostat` within ±1% |
| M4 | `storage/` — ring-buffer with level markers + tier aggregation (irregular samples) + crash-recovery | fsck tool and recovery with no more than 1 record lost |
| M4.1 | **Tick scheduler** — L1/L2/L3 adaptive logic + anomaly detector (threshold-based) + hysteresis | Unit tests: escalation/de-escalation scenarios; integration test: WS connect → L3 within < 2s |
| M4.2 | **Hot buffer** — lock-free in-memory ring, catch-up dump on client connect, grace period | WS client receives 300 records catch-up on connect; hot buffer resets 60s after disconnect |
| M5 | `web/` — REST endpoints with resolution zone markers | `/api/history` returns records with `level` field and `L1/L2/L3` zones |
| M6 | WS hub + embedded SPA (with resolution zone visualization) | Dashboard shows L1 zones with gray background, L3 zones — full charts |
| M7 | `auth/` Argon2id + sessions | Auth tests, login rate-limit |
| M8 | `tui/` on ncurses/notcurses | CPU, mem, net, disk, load, processes — like btop but lighter |
| M8.1 | **Lua rule engine** — embedded Lua 5.4 VM, sandbox, `watch()`/`alert()`/`exec()`/`escalate()` builtins, per-tick eval, sustain counters, cooldown | 20 example rules pass; sandbox blocks `io`/`os`; `exec` runs with timeout; scheduler escalation on rule fire |
| M8.2 | **AI rule suggestions (Tier A)** — local heuristic engine, `budyk suggest-rules` CLI command | Given 7d of synthetic history, produces valid Lua `watch()` calls that parse and evaluate without errors |
| M9 | Packaging: FreeBSD port (`Makefile` for `/usr/ports/sysutils/budyk`), rc.d script, pkg-message; Linux — deb/rpm | `pkg install budyk` works on FreeBSD 13/14/15 |
| M10 | Documentation, man page, changelog, 1.0.0 | Release |

---

## 5. Acceptance Criteria

1. **Full metric set** (table in §2.1) — all `struct Sample` fields populated on FreeBSD 13/14/15.
2. **Idle performance (adaptive, 0 clients)**: CPU ≈ 0% (1 sysctl every 5 min), disk I/O < 1 KB/min. RSS ≤ 10 MB.
3. **Active client performance**: RSS ≤ 25 MB, CPU ≤ 1% on a 4-core host at L3 (1 Hz) with one WS client.
4. **L1→L3 escalation**: ≤ 2 seconds from WS client connect to first sample at 1 Hz.
5. **Hot buffer catch-up**: WS client receives ≤ 300 history records within ≤ 100ms on connect.
6. **Binary footprint**: ≤ 5 MB stripped static on amd64.
7. **Stability**: 72h soak test under nemesis (kill -9 every N minutes) — recovery without ring-buffer corruption. Valgrind/ASan clean.
8. **Cross-compilation**: `cmake --toolchain=freebsd-amd64.cmake` and `freebsd-aarch64.cmake` from a Linux host (via cross-clang or FreeBSD sysroot).
9. **Security**: cppcheck clean, clang-tidy clean, `-fsanitize=address,undefined` in CI, Argon2id parameters no lower than OWASP 2024 recommendations.
10. **Tests**: `core/` and `storage/` coverage ≥ 85% by lines (gcov/lcov); golden tests for parsers; scenario tests for tick scheduler (escalation/de-escalation).
11. **Platforms**: CI green on FreeBSD 13.4, 14.2, 15-CURRENT + Linux (Ubuntu 24.04) + Cirrus CI.
12. **Documentation**: Doxygen without warnings, man page, README with FreeBSD quickstart.
13. **Rule engine**: Lua 5.4 sandbox; 100 `watch()` rules evaluated per tick within ≤ 1ms; `exec` actions run with configurable timeout (default 30s); sustain counters survive level transitions (L3→L1→L3); sandbox blocks `io`, `os`, `loadfile`.
14. **AI suggestions (Tier A)**: `budyk suggest-rules` CLI command completes within 5s on 30 days of Tier 2 data; output is valid Lua that parses without errors; suggested thresholds are within plausible range.

---

## 6. Risks and Mitigation

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Reference systems mutate, their APIs/protocols become outdated | Medium | Low | Our project does not depend on them — they are only idea sources |
| AGPL contamination from accidental code copying from kula/Netdata | Low | Critical | Code review on every PR, SPDX headers in all files, `scancode-toolkit` in CI |
| `devstat`/`kinfo_proc` API changes between FreeBSD 13→14→15 | Medium | Medium | `#ifdef` branching by `__FreeBSD_version`, CI on 13/14/15-CURRENT |
| Memory safety (buffer overflow, use-after-free) in C/C++ | High | Critical | `-fsanitize=address,undefined` in CI, Valgrind on every PR, `-D_FORTIFY_SOURCE=2`, `-fstack-protector-strong`. Collectors (C) — maximally simple, no dynamic allocations |
| NVML on FreeBSD — limited support | High | Low | `#ifdef HAVE_NVML`, disabled by default |
| mongoose/libyaml dependency disappears or becomes unmaintained | Low | Medium | Both libraries vendored in `third_party/`, can be forked |
| capsicum sandboxing issues on FreeBSD with mmap+sysctl | Low | Low | Defer sandbox to v1.1 |
| C++ ABI incompatibility between clang/gcc across FreeBSD versions | Low | Medium | Static linking by default, `-static-libstdc++` |
| Rule `exec` action runs arbitrary commands as daemon user | Medium | High | Document security implications; recommend running daemon as unprivileged user; `exec` disabled by default, enabled via `--enable-exec` flag; commands run with timeout + resource limits (setrlimit) |
| LLM API sends metric data externally (Tier B) | Low | Medium | Only anonymized statistical summaries sent (no raw samples, no hostnames); Tier B is opt-in, off by default, requires explicit `--ai` flag + API key |

---

## 7. Out of Scope (v1.0)

- Cluster federation (multiple nodes → single dashboard)
- Prometheus/OpenMetrics export
- Notification delivery (email, Slack, webhook, PagerDuty) — v1.0 rules fire `alert` to log + WS event only; external notification integrations deferred to v1.1
- AI rule suggestions Tier B (LLM-assisted) — architecture prepared, implementation deferred to v1.1; Tier A (local heuristics) is in scope
- AI in dashboard (interactive rule builder) — v1.2+
- Jails/VNET-aware metrics (deferred to v1.1 — FreeBSD-specific, separate topic)
- ZFS-specific metrics (`kstat.zfs.*` — rich separate topic, v1.2)
- Windows/macOS

---

## 8. Next Steps

Upon approval of this spec:

1. Create repository `budyk` with CMake skeleton (M0), BSD-3-Clause in every file.
2. Assign one person to Phase 1 (reference study) and another to the FreeBSD collector prototype.
3. Start Phase 1 — expected duration 1–2 weeks per engineer.
4. In parallel, begin collecting sysctl values on a real FreeBSD host (13.x, 14.x, 15-CURRENT) to validate §2.1 mapping.
5. Decide on HTTP library (mongoose vs libwebsockets) — write PoC echo-server on both, measure footprint and complexity.

---

## Appendix A. Prior Art: Software with Similar Collection Models

Our 3-level adaptive model is not entirely new. Below is a registry of existing systems that implement elements of this approach. None of them implements all three elements (multi-level adaptive collection + anomaly-triggered escalation + on-demand hot buffer) together.

### A.1. Pure On-Demand (model 3: collect only when a client is present)

| Software | Language | How It Works | What to Take |
|----------|----------|--------------|--------------|
| **htop / btop / top** | C / C++ | Classic TUI monitors: collect and display metrics **only while running**. No background daemon, no history. Close — data lost. | Validation that the on-demand model works and is in demand (btop — one of the most starred TUIs on GitHub). |
| **Glances** (server mode `-w`) | Python / psutil | Web server starts, but collectors always run while the process is alive. Without DB export — data in-memory, lost on restart. | REST API, WebSocket stream, cross-platform via `psutil`. |
| **Netdata issue #238** (2016, earthgecko) | — | "Power saving mode" proposal: start Netdata collection only on HTTP request to port. Implemented as a hack via `nc → start netdata`. Upstream **rejected** — Netdata fundamentally builds on always-on 1 Hz. | Validation of the idea: if 99.99% of the time nobody is watching, always-on is waste. |

### A.2. Fixed Tiered Collection (no adaptivity)

| Software | Language | How It Works | What to Take |
|----------|----------|--------------|--------------|
| **Kula** | Go | 3 storage tiers (1s, 1m, 5m ring-buffers), but **collection always at 1 Hz**. Tiers are aggregation at storage, not at collection. | Ring-buffer design, JSON codec (its weakness — we replace with msgpack/binary). |
| **Netdata** | C | 3 storage tiers (1s, 1m/5m aggregates), always 1 Hz collection. RAM-mode support for child nodes (no disk I/O). ML anomaly detection at Parent level. | Tier architecture, parent-child streaming. ML for anomaly — but Netdata does not reduce collection frequency in response to anomaly, it alerts instead. |
| **sysstat / sar** | C | Cron-based: fixed interval (10 min default), binary files `/var/log/sa/saDD`, daily rotation. No adaptivity. | sa-file format — example of compact binary metric storage. |
| **atop** | C | Fixed interval (10 min default), binary snapshots in `/var/log/atop/`. Post-mortem replay (`atop -r`). | Post-mortem replay concept from binary logs. |
| **RRDtool** (Round Robin Database) | C | Classic tiered ring-buffer: multiple RRAs (Round Robin Archives) with different consolidation (AVG, MAX, MIN) and step. Fixed file size. Used in Cacti, Munin, collectd. | Ring-buffer tier pioneer. Format well-studied. |
| **Prometheus** | Go | Pull model with fixed scrape_interval (15s default). Recording rules for aggregation to coarser intervals. Downsampling via Thanos/Cortex. | Recording rules — analogous to our L3→L2→L1 aggregation. |

### A.3. Adaptive Sampling (interval changes by condition)

| Software | Language | How It Works | What to Take |
|----------|----------|--------------|--------------|
| **AdaM** (University of Cyprus, IEEE TSC 2018) | Python/C | IoT framework: dynamically changes collection frequency and filtering based on current metric variability. If metric is stable — interval grows; if volatile — it shrinks. Result: 74% data volume reduction, 71% energy savings, >89% accuracy. | Closest academic analog to our model. Confidence-based estimation algorithm for choosing T. |
| **Azure Application Insights Adaptive Sampling** | .NET | Automatic adjustment of request sampling percentage to maintain a volume budget. Not about host metrics — about APM traces. | "IOPS budget / record budget" idea — transferable to our storage. |
| **Datadog Adaptive Sampling** | — | Auto-adjusting sampling rate for distributed traces based on budget. Up to 800 service/env combinations. | Per-service sampling rate mechanism — analogy to per-metric level in our L1/L2/L3. |
| **FAST** (PID controller for adaptive sampling) | Python | Uses a PID controller for optimal collection interval selection. Aggressive — large intervals during stable periods. Accuracy lower than AdaM on volatile signals. | PID approach as an alternative to the threshold-based anomaly detector for v1.1+ |
| **Meng et al. (violation-likelihood)** | — | For cloud networks: increases monitoring frequency when metric value approaches a user-defined threshold. Otherwise — fixed rate. | Direct analogy to our L1→L2 escalation by thresholds. |

### A.4. Anomaly-Triggered Escalation (collection accelerates on problem detection)

| Software | Language | How It Works | What to Take |
|----------|----------|--------------|--------------|
| **Dynatrace Davis AI** | proprietary | Auto-adaptive thresholds + seasonal baselines. On anomaly detection — automatic topology binding, root-cause analysis. Does not change agent collection frequency, but changes processing intensity. | "Investigation escalation" concept — on anomaly detection, engage more analysis resources (in our case — more metrics via L2→L3). |
| **Netdata ML** | C + Python | Anomaly detection at edge. ML models trained on Parent node, inference on Child. Anomaly → alert, but **not** collection frequency change. | ML anomaly rate — can be used as an additional L1→L2 trigger in future versions (v1.1+). |
| **Chowdhury et al. (policy-violation framework)** | — | Adjusts monitoring intensity on user policy violation. | Policy-driven escalation concept — extension of our threshold model. |

### A.5. Conclusion: Our Position

None of the identified tools combines all three elements:

1. **Multi-level adaptive collection** (L1/L2/L3 with different frequency and metric sets)
2. **Anomaly-triggered escalation** (automatic transition to more frequent collection upon problem detection)
3. **On-demand hot buffer** (RAM-only for live streaming with zero disk I/O in idle)

AdaM (§A.3) is the closest in spirit, but it targets IoT with battery constraints, not server monitoring. Netdata issue #238 shows the "always-on overhead" problem is recognized by the community, but major projects do not solve it — instead, they optimize the footprint of the always-on approach.

Our project occupies an empty niche: **a minimal self-contained monitor that truly sleeps when not needed and instantly wakes up when needed**.

---

*Document — version 0.7, draft. v0.7: project named "budyk"; rule engine rewritten from custom YAML DSL to embedded Lua 5.4; Lua sandbox, `watch()` API; AI Tier A generates Lua `watch()` calls.*
