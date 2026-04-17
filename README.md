# budyk

**Lightweight server monitoring with adaptive collection.**

budyk (Ukrainian "будик" — alarm clock) is a self-contained monitoring daemon
for FreeBSD servers. Single binary, no external dependencies, no database.

## Key idea

Most monitoring tools collect metrics every second regardless of whether anyone
is watching. budyk uses a **3-level adaptive model**:

- **L1 — Heartbeat** (every 5 min): minimal metrics, ~56 KB/day on disk
- **L2 — Watchful** (15–60s): extended metrics, activates on anomaly detection
- **L3 — Active** (1 Hz): full metrics, activates when a client connects

When nobody is watching and the system is healthy, budyk truly sleeps.
When you open the dashboard or an anomaly occurs, it instantly wakes up.

## Quick start

```sh
# Build
cmake -B build && cmake --build build

# Run
./build/src/budyk serve --config config.example.yaml

# Dashboard at http://localhost:8080

# Terminal UI
./build/src/budyk tui
```

## Monitoring rules (Lua)

Rules are written in Lua — simple, readable, powerful:

```lua
watch("high_cpu", {
    when = function() return cpu.total_percent > 90 end,
    for_ticks = 5,
    severity = "warning",
    action = alert
})
```

AI-assisted rule generation (from collected history):
```sh
budyk suggest-rules --window 7d --output suggested-rules.lua
```

## Platforms

- **FreeBSD** 13.x / 14.x / 15.x (primary target)
- Linux (secondary)
- NetBSD, OpenBSD (nice-to-have)

## License

BSD-3-Clause
