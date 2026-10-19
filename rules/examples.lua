-- SPDX-License-Identifier: BSD-3-Clause
-- budyk example rules
-- Point `rules.path` in config.yaml at a copy of this file.
--
-- watch(name, opts):
--   when      — function: true to fire (after for_ticks), false to reset
--               the streak, nil to skip the tick (the streak is kept)
--   action    — alert (default), "log", escalate, a function, or a list
--               of them run in order: { alert, exec("/path/to/script") }
--   severity  — "info" / "warning" (default) / "critical"
--   message   — alert / log text (default: the rule name)
--   for_ticks — consecutive true ticks before firing (default 1)
--   cooldown  — ticks to stay quiet after firing (default: for_ticks)

-- Simple threshold
watch("high_cpu", {
    when      = function() return cpu.total_percent > 90 end,
    for_ticks = 5,
    cooldown  = 60,
    severity  = "warning",
    message   = "CPU above 90% for 5 ticks",
})

-- Memory critical
watch("memory_critical", {
    when      = function() return mem.available_percent < 5 end,
    for_ticks = 3,
    cooldown  = 60,
    severity  = "critical",
    message   = "Less than 5% of memory available",
})

-- Computed threshold — load relative to core count
watch("overloaded", {
    when = function()
        return load.avg_1m > cpu.count * 2
    end,
    for_ticks = 10,
    cooldown  = 60,
    severity  = "warning",
    message   = "1-minute load above twice the core count",
})

-- Swap pressure under load (compound condition), with a function action
-- that puts live values into the message.
watch("swap_under_load", {
    when = function()
        return swap.used_percent > 80 and load.avg_1m > cpu.count
    end,
    for_ticks = 3,
    cooldown  = 60,
    action    = function()
        alert("swap_under_load", "critical",
              string.format("swap %.0f%% used, load %.2f",
                            swap.used_percent, load.avg_1m))
    end,
})

-- Log only, no notification
watch("disk_write_burst", {
    when      = function() return disk.write_bytes_per_sec > 500 * 1024 * 1024 end,
    for_ticks = 3,
    cooldown  = 30,
    action    = "log",
    message   = "disk writes above 500 MB/s",
})

-- A disk that may not be there: nil skips the tick instead of counting
-- as "fine", so a device that comes and goes keeps its streak.
watch("sda_busy", {
    when      = function()
        local d = disk.devices.sda
        return d and d.busy_percent > 90
    end,
    for_ticks = 10,
    severity  = "warning",
    message   = "sda busy above 90% for 10 ticks",
})

-- Several actions: alert, then collect at 1 Hz (L3) for a minute to see
-- what happens next.
watch("load_spike", {
    when      = function() return load.avg_1m > cpu.count * 4 end,
    for_ticks = 2,
    cooldown  = 60,
    severity  = "critical",
    message   = "1-minute load above four times the core count",
    action    = { alert, escalate },
})

-- With rules.exec.enabled (or --enable-exec), a command can be one of the
-- actions. exec() written here runs nothing; it is checked now and run
-- when the rule fires:
--   action = { alert, exec("/usr/local/bin/drop-caches.sh") },
