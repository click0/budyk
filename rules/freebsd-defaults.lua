-- SPDX-License-Identifier: BSD-3-Clause
-- budyk FreeBSD-specific default rules
-- These cover common FreeBSD server scenarios.

watch("high_cpu", {
    when = function() return cpu.total_percent > 85 end,
    for_ticks = 10,
    severity = "warning",
    action = alert
})

watch("memory_low", {
    when = function() return mem.available_percent < 10 end,
    for_ticks = 5,
    severity = "warning",
    action = alert
})

watch("swap_active", {
    when = function() return swap.used_percent > 50 end,
    for_ticks = 5,
    severity = "warning",
    action = { alert, escalate }
})

-- TODO: ZFS ARC pressure (v1.2)
-- TODO: jail resource limits (v1.1)
