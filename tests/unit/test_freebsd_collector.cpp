// SPDX-License-Identifier: BSD-3-Clause
// Exercises the FreeBSD collector C functions against the running host.
// Only added to the build when BUDYK_PLATFORM == freebsd (see tests/CMakeLists.txt).
//
// At present only the CPU collector is implemented; memory / load /
// uptime / disk / net are still stubs and will be added as they land.

#include "core/sample_c.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

int main() {
    // 1. CPU delta — first call baselines (0%), second call yields a
    //    valid percentage in [0,100], cpu.count > 0, ctx remembers state.
    {
        budyk_cpu_ctx_c ctx;
        std::memset(&ctx, 0, sizeof(ctx));

        budyk_sample_c s1;
        std::memset(&s1, 0, sizeof(s1));
        assert(budyk_collect_cpu_freebsd(&ctx, &s1) == 0);
        assert(s1.cpu.count > 0);
        assert(s1.cpu.total_percent == 0.0);  // no baseline yet
        assert(ctx.has_prev == 1);

        // Burn a little CPU so the second snapshot has real work to
        // attribute, then re-sample.
        volatile uint64_t acc = 0;
        for (uint64_t i = 0; i < 2'000'000ULL; ++i) acc += i;
        (void)acc;

        budyk_sample_c s2;
        std::memset(&s2, 0, sizeof(s2));
        assert(budyk_collect_cpu_freebsd(&ctx, &s2) == 0);
        assert(s2.cpu.count == s1.cpu.count);
        assert(s2.cpu.total_percent >= 0.0);
        assert(s2.cpu.total_percent <= 100.0);
    }

    // 2. Null-pointer guard.
    {
        budyk_cpu_ctx_c ctx{};
        budyk_sample_c  s{};
        assert(budyk_collect_cpu_freebsd(nullptr, &s) != 0);
        assert(budyk_collect_cpu_freebsd(&ctx, nullptr) != 0);
    }

    std::printf("test_freebsd_collector: PASS\n");
    return 0;
}
