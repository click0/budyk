-- SPDX-License-Identifier: BSD-3-Clause
-- budyk FreeBSD-specific default rules
-- These cover common FreeBSD server scenarios. See examples.lua for the
-- full list of watch() options.

watch("high_cpu", {
    when      = function() return cpu.total_percent > 85 end,
    for_ticks = 10,
    cooldown  = 60,
    severity  = "warning",
    message   = "CPU above 85% for 10 ticks",
})

watch("memory_low", {
    when      = function() return mem.available_percent < 10 end,
    for_ticks = 5,
    cooldown  = 60,
    severity  = "warning",
    message   = "Less than 10% of memory available",
})

watch("swap_active", {
    when      = function() return swap.used_percent > 50 end,
    for_ticks = 5,
    cooldown  = 60,
    severity  = "warning",
    message   = "More than half of swap in use",
})

-- TODO: ZFS ARC pressure (v1.2)
-- TODO: jail resource limits (v1.1)
