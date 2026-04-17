-- SPDX-License-Identifier: BSD-3-Clause
-- budyk example rules
-- Place in /usr/local/etc/budyk/rules.lua or include via config.yaml

-- Simple threshold
watch("high_cpu", {
    when = function() return cpu.total_percent > 90 end,
    for_ticks = 5,
    severity = "warning",
    action = alert
})

-- Memory critical
watch("memory_critical", {
    when = function() return mem.available_percent < 5 end,
    for_ticks = 3,
    severity = "critical",
    action = { alert, escalate }
})

-- Computed threshold — load relative to core count
watch("overloaded", {
    when = function()
        return load.avg_1m > cpu.count * 2
    end,
    for_ticks = 10,
    severity = "warning",
    action = alert
})

-- Swap pressure under load (compound condition)
watch("swap_under_load", {
    when = function()
        return swap.used_percent > 80 and load.avg_1m > cpu.count
    end,
    for_ticks = 3,
    severity = "critical",
    action = { alert, escalate }
})

-- Per-interface network errors (dynamic iteration)
-- Uncomment when net.interfaces is implemented:
--
-- for name, iface in pairs(net.interfaces) do
--     watch("rx_errors_" .. name, {
--         when = function() return iface.rx_errors_per_sec > 100 end,
--         for_ticks = 5,
--         action = alert
--     })
-- end
