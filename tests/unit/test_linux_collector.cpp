// SPDX-License-Identifier: BSD-3-Clause
// Exercises the Linux collector C functions against the running host.
// Only added to the build when BUDYK_PLATFORM == linux (see tests/CMakeLists.txt).

#include "core/sample_c.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    // 1. Memory: MemTotal / MemAvailable present and sane.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_memory_linux(&s);
        assert(rc == 0);
        assert(s.mem.total     > 0);
        assert(s.mem.available <= s.mem.total);
        assert(s.mem.available_percent >= 0.0);
        assert(s.mem.available_percent <= 100.0);
        // Swap is optional: may be zero on stripped CI containers.
        if (s.swap.total > 0) {
            assert(s.swap.used <= s.swap.total);
            assert(s.swap.used_percent >= 0.0);
            assert(s.swap.used_percent <= 100.0);
        }
    }

    // 2. Uptime: non-zero, positive.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_uptime_linux(&s);
        assert(rc == 0);
        assert(s.uptime_seconds > 0.0);
    }

    // 3. Load average: three non-negative numbers.
    {
        budyk_sample_c s;
        std::memset(&s, 0, sizeof(s));
        int rc = budyk_collect_load_linux(&s);
        assert(rc == 0);
        assert(s.load.avg_1m  >= 0.0);
        assert(s.load.avg_5m  >= 0.0);
        assert(s.load.avg_15m >= 0.0);
    }

    // 4. Null-pointer guard.
    {
        assert(budyk_collect_memory_linux(nullptr) != 0);
        assert(budyk_collect_uptime_linux(nullptr) != 0);
        assert(budyk_collect_load_linux  (nullptr) != 0);
    }

    std::printf("test_linux_collector: PASS\n");
    return 0;
}
