// SPDX-License-Identifier: BSD-3-Clause
#include "core/sample.h"
#include "storage/tier_manager.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

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

    // 8. query() — chronological window read from a tier ring.
    {
        const std::string d = mkdtmp();
        TierManager tm;
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);

        // Ten L3 samples at ts = 1000, 1100, ... 1900.
        for (int i = 0; i < 10; ++i) {
            assert(tm.store(mk(Level::L3, 1000ULL + i * 100)) == 0);
        }

        // Full read, no bounds → all 10, oldest-first.
        {
            std::vector<Sample> out;
            const int n = tm.query(1, /*since*/0, /*until*/0, 100, &out);
            assert(n == 10);
            assert(out.size() == 10);
            assert(out.front().timestamp_nanos == 1000);
            assert(out.back().timestamp_nanos  == 1900);
            // strictly increasing
            for (size_t i = 1; i < out.size(); ++i)
                assert(out[i].timestamp_nanos > out[i - 1].timestamp_nanos);
        }

        // since filter — only ts >= 1500 (five samples: 1500..1900).
        {
            std::vector<Sample> out;
            const int n = tm.query(1, 1500, 0, 100, &out);
            assert(n == 5);
            assert(out.front().timestamp_nanos == 1500);
            assert(out.back().timestamp_nanos  == 1900);
        }

        // since + until window — [1200, 1400] inclusive → 3 samples.
        {
            std::vector<Sample> out;
            const int n = tm.query(1, 1200, 1400, 100, &out);
            assert(n == 3);
            assert(out.front().timestamp_nanos == 1200);
            assert(out.back().timestamp_nanos  == 1400);
        }

        // limit caps to the NEWEST max_records.
        {
            std::vector<Sample> out;
            const int n = tm.query(1, 0, 0, 3, &out);
            assert(n == 3);
            assert(out.front().timestamp_nanos == 1700);
            assert(out.back().timestamp_nanos  == 1900);
        }

        // Empty tier (tier2 never written) → 0, out untouched.
        {
            std::vector<Sample> out;
            const int n = tm.query(2, 0, 0, 100, &out);
            assert(n == 0);
            assert(out.empty());
        }

        // Bad args: invalid tier, zero max, null out, before-init.
        {
            std::vector<Sample> out;
            assert(tm.query(0, 0, 0, 100, &out) == -1);
            assert(tm.query(4, 0, 0, 100, &out) == -1);
            assert(tm.query(1, 0, 0, 0,   &out) == -1);
            assert(tm.query(1, 0, 0, 100, nullptr) == -1);
        }

        tm.close();
        // Query on a closed manager is rejected.
        {
            std::vector<Sample> out;
            assert(tm.query(1, 0, 0, 100, &out) == -1);
        }
        rmrf(d);
    }

    // N. Custom-level rings: a sample at a custom level goes to its own
    //    level-<name>.ring and comes back through query_level(). The ring
    //    is found by name, so after a config reorder gives the level a new
    //    id, its old samples are reported with the new id.
    {
        const std::string d = mkdtmp();
        {
            TierManager tm;
            assert(tm.init(d.c_str(), 1, 1, 1, {LevelRingSpec{4, "burst", 1}}) == 0);
            assert(::access((d + "/level-burst.ring").c_str(), F_OK) == 0);
            assert(tm.store(mk(static_cast<Level>(4), 100)) == 0);
            assert(tm.store(mk(static_cast<Level>(4), 200)) == 0);
            assert(tm.store(mk(static_cast<Level>(5), 300)) == -2);   // no ring for 5
            std::vector<Sample> out;
            assert(tm.query_level(static_cast<Level>(4), 0, 0, 10, &out) == 2);
            assert(out[0].timestamp_nanos == 100 && out[1].timestamp_nanos == 200);
            assert(out[0].level == static_cast<Level>(4));
            std::vector<Sample> none;
            assert(tm.query_level(static_cast<Level>(7), 0, 0, 10, &none) == -1);
            // Built-ins through query_level map to the right tier.
            assert(tm.store(mk(Level::L3, 400)) == 0);
            std::vector<Sample> l3;
            assert(tm.query_level(Level::L3, 0, 0, 10, &l3) == 1);
            tm.close();
        }
        {
            TierManager tm;   // same name, new id
            assert(tm.init(d.c_str(), 1, 1, 1, {LevelRingSpec{6, "burst", 1}}) == 0);
            std::vector<Sample> out;
            assert(tm.query_level(static_cast<Level>(6), 0, 0, 10, &out) == 2);
            assert(out[0].level == static_cast<Level>(6));
            tm.close();
        }
        rmrf(d);
    }

    // N+1. query_all(): every ring merged oldest-first with each sample's
    //      level; over the limit it's thinned by time, keeping both ends
    //      of the window rather than only the newest samples.
    {
        const std::string d = mkdtmp();
        TierManager tm;
        assert(tm.init(d.c_str(), 1, 1, 1, {LevelRingSpec{4, "burst", 1}}) == 0);
        assert(tm.store(mk(Level::L1, 100)) == 0);
        assert(tm.store(mk(static_cast<Level>(4), 150)) == 0);
        assert(tm.store(mk(Level::L2, 200)) == 0);
        assert(tm.store(mk(Level::L3, 300)) == 0);
        std::vector<Sample> out;
        assert(tm.query_all(0, 0, 10, &out) == 4);
        assert(out[0].timestamp_nanos == 100 && out[0].level == Level::L1);
        assert(out[1].timestamp_nanos == 150 && out[1].level == static_cast<Level>(4));
        assert(out[2].level == Level::L2 && out[3].level == Level::L3);
        std::vector<Sample> win;
        assert(tm.query_all(150, 250, 10, &win) == 2);    // window bounds

        // 100 raw samples, limit 10 → 10 points spread over the window.
        for (uint64_t i = 0; i < 100; ++i) {
            assert(tm.store(mk(Level::L3, 1000 + i)) == 0);
        }
        std::vector<Sample> thin;
        const int n = tm.query_all(1000, 0, 10, &thin);
        assert(n > 0 && n <= 10);
        assert(thin.front().timestamp_nanos < 1020);       // start covered
        assert(thin.back().timestamp_nanos == 1099);       // end covered
        for (size_t i = 1; i < thin.size(); ++i) {
            assert(thin[i - 1].timestamp_nanos < thin[i].timestamp_nanos);
        }
        tm.close();
        rmrf(d);
    }

    // N+2. query_all() on a ring that has wrapped: the binary search finds
    //      exact window bounds, and an unbounded window starts at the
    //      oldest record still in the ring.
    {
        const std::string d = mkdtmp();
        TierManager tm;
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);         // 1 MiB ≈ 3.8k records
        const uint64_t n = 5000;
        for (uint64_t i = 0; i < n; ++i) {
            assert(tm.store(mk(Level::L3, 10000 + i)) == 0);
        }
        std::vector<Sample> win;
        assert(tm.query_all(12000, 12100, 500, &win) == 101);
        assert(win.front().timestamp_nanos == 12000);
        assert(win.back().timestamp_nanos  == 12100);

        std::vector<Sample> all;
        const int k = tm.query_all(0, 0, 10, &all);
        assert(k > 0 && k <= 10);
        assert(all.back().timestamp_nanos == 10000 + n - 1);
        std::vector<Sample> oldest;                        // oldest surviving record
        assert(tm.query(1, 0, 0, 100000, &oldest) > 0);
        assert(all.front().timestamp_nanos == oldest.front().timestamp_nanos);
        assert(oldest.front().timestamp_nanos > 10000);   // the ring did wrap
        tm.close();
        rmrf(d);
    }

    // N+3. query_all() thinning across rings. Each ring already comes back
    //      spread to at most `limit` records, so the merge-level thinning
    //      only runs when several rings together exceed the limit. Three
    //      interleaved rings × 40 samples, limit 12.
    {
        const std::string d = mkdtmp();
        TierManager tm;
        assert(tm.init(d.c_str(), 1, 1, 1, {LevelRingSpec{4, "burst", 1}}) == 0);
        for (uint64_t i = 0; i < 40; ++i) {
            assert(tm.store(mk(Level::L3, 1000 + 4 * i)) == 0);
            assert(tm.store(mk(Level::L2, 1001 + 4 * i)) == 0);
            assert(tm.store(mk(static_cast<Level>(4), 1002 + 4 * i)) == 0);
        }
        std::vector<Sample> out;
        const int n = tm.query_all(0, 0, 12, &out);
        assert(n > 0 && n <= 12);
        for (size_t i = 1; i < out.size(); ++i) {
            assert(out[i - 1].timestamp_nanos < out[i].timestamp_nanos);
        }
        // Each of the 12 slices of the window (1000..1158, ~13.25 wide)
        // keeps its newest sample: the first point lies within the first
        // slice, the last is the newest sample of all.
        assert(out.front().timestamp_nanos < 1000 + 14);
        assert(out.back().timestamp_nanos == 1002 + 4 * 39);
        // tier 3 (L1) through the numeric query, for completeness.
        assert(tm.store(mk(Level::L1, 5000)) == 0);
        std::vector<Sample> l1;
        assert(tm.query(3, 0, 0, 10, &l1) == 1 && l1[0].level == Level::L1);
        tm.close();
        rmrf(d);
    }

    // N+4. A custom ring that no longer matches its configured size (the
    //      user changed storage_mb) fails init() with -9 and closes every
    //      ring it had opened; init() with the original size still works.
    {
        const std::string d = mkdtmp();
        {
            TierManager tm;
            assert(tm.init(d.c_str(), 1, 1, 1, {LevelRingSpec{4, "burst", 1}}) == 0);
            assert(tm.store(mk(static_cast<Level>(4), 100)) == 0);
            tm.close();
        }
        {
            TierManager tm;
            assert(tm.init(d.c_str(), 1, 1, 1, {LevelRingSpec{4, "burst", 2}}) == -9);
            std::vector<Sample> out;
            assert(tm.query_level(Level::L3, 0, 0, 10, &out) == -1);   // not ready
        }
        {
            TierManager tm;
            assert(tm.init(d.c_str(), 1, 1, 1, {LevelRingSpec{4, "burst", 1}}) == 0);
            std::vector<Sample> out;
            assert(tm.query_level(static_cast<Level>(4), 0, 0, 10, &out) == 1);
            tm.close();
        }
        rmrf(d);
    }

    std::printf("test_tier_manager: PASS\n");
    return 0;
}
