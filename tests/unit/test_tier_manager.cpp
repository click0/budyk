// SPDX-License-Identifier: BSD-3-Clause
#include "core/sample.h"
#include "storage/tier_manager.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

using namespace budyk;

static Sample mk(Level level, uint64_t ts) {
    Sample s{};
    s.timestamp_nanos    = ts;
    s.level              = level;
    s.cpu.total_percent  = 42.0;
    s.cpu.count          = 4;
    s.mem.total          = 16ULL << 30;
    s.mem.available      = 4ULL  << 30;
    s.uptime_seconds     = 1234.5;
    return s;
}

static std::string mkdtmp() {
    char tmpl[] = "/tmp/budyk_tm_XXXXXX";
    char* dir = mkdtemp(tmpl);
    assert(dir != nullptr);
    return dir;
}

static void rmrf(const std::string& d) {
    const std::string cmd = "rm -rf " + d;
    int rc = std::system(cmd.c_str());
    (void)rc;
}

int main() {
    // 1. init() rejects nullptr.
    {
        TierManager tm;
        assert(tm.init(nullptr) != 0);
    }

    // 2. init() rejects double-init on the same instance.
    {
        const std::string d = mkdtmp();
        TierManager tm;
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);
        assert(tm.init(d.c_str(), 1, 1, 1) != 0);   // already initialised
        tm.close();
        rmrf(d);
    }

    // 3. init() with a non-existent directory fails cleanly.
    {
        TierManager tm;
        assert(tm.init("/nonexistent/budyk/path/xyzzy", 1, 1, 1) != 0);
    }

    // 4. Routing — store() sends each level to its tier; counts reflect.
    {
        const std::string d = mkdtmp();
        TierManager tm;
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);
        assert(tm.tier1_count() == 0);
        assert(tm.tier2_count() == 0);
        assert(tm.tier3_count() == 0);

        // L3 → tier1 (raw)
        for (int i = 0; i < 3; ++i) {
            assert(tm.store(mk(Level::L3, 1000ULL + i)) == 0);
        }
        // L2 → tier2 (1-min aggregate)
        for (int i = 0; i < 2; ++i) {
            assert(tm.store(mk(Level::L2, 2000ULL + i)) == 0);
        }
        // L1 → tier3 (5-min aggregate)
        assert(tm.store(mk(Level::L1, 3000ULL)) == 0);

        assert(tm.tier1_count() == 3);
        assert(tm.tier2_count() == 2);
        assert(tm.tier3_count() == 1);

        tm.close();
        rmrf(d);
    }

    // 5. store() before init() is rejected (no segfault, returns negative).
    {
        TierManager tm;
        Sample s = mk(Level::L3, 1);
        assert(tm.store(s) != 0);
    }

    // 6. close() then re-init is allowed (data_dir reused).
    {
        const std::string d = mkdtmp();
        TierManager tm;
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);
        assert(tm.store(mk(Level::L3, 1)) == 0);
        assert(tm.tier1_count() == 1);
        tm.close();

        // Re-open same dir — header validation must accept the existing
        // ring files, write_idx persists.
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);
        assert(tm.tier1_count() == 1);                // count carried across close
        assert(tm.store(mk(Level::L3, 2)) == 0);
        assert(tm.tier1_count() == 2);
        tm.close();
        rmrf(d);
    }

    // 7. Capacity defaults — a 250 MB tier1 should produce a much larger
    //    capacity than the 1 MB tier3, and no file ops should fail.
    {
        const std::string d = mkdtmp();
        TierManager tm;
        // Sane production-ish defaults; just verifies no I/O surprises
        // on bigger numbers.
        assert(tm.init(d.c_str(), /*tier1*/16, /*tier2*/8, /*tier3*/4) == 0);
        assert(tm.store(mk(Level::L3, 1)) == 0);
        assert(tm.store(mk(Level::L2, 2)) == 0);
        assert(tm.store(mk(Level::L1, 3)) == 0);
        tm.close();
        rmrf(d);
    }

    std::printf("test_tier_manager: PASS\n");
    return 0;
}
