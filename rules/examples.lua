-- SPDX-License-Identifier: BSD-3-Clause
-- budyk example rules
-- Point `rules.path` in config.yaml at a copy of this file.
--
-- watch(name, opts):
--   when      — function returning true when the rule should fire
--   action    — "alert" (default), "log", or a function
--   severity  — "info" / "warning" (default) / "critical"
--   message   — alert / log text (default: the rule name)
--   for_ticks — consecutive true ticks before firing (default 1)
--   cooldown  — ticks to stay quiet after firing (default 0)

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
