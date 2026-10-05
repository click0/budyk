// SPDX-License-Identifier: BSD-3-Clause
#include "hot_buffer/hot_buffer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace budyk;

static Sample mk(uint64_t ts) {
    Sample s{};
    s.timestamp_nanos = ts;
    s.level           = Level::L3;
    return s;
}

int main() {
    // 1. Empty buffer — nothing to dump.
    {
        HotBuffer b(10);
        Sample out[10]{};
        assert(b.size() == 0);
        assert(b.dump(out, 10) == 0);
    }

    // 2. Partial fill — dump oldest-first.
    {
        HotBuffer b(5);
        for (uint64_t i = 1; i <= 3; ++i) b.push(mk(i));
        assert(b.size() == 3);

        Sample out[5]{};
        assert(b.dump(out, 5) == 3);
        assert(out[0].timestamp_nanos == 1);
        assert(out[1].timestamp_nanos == 2);
        assert(out[2].timestamp_nanos == 3);
    }

    // 3. Exactly full, head wrapped to 0.
    {
        HotBuffer b(4);
        for (uint64_t i = 1; i <= 4; ++i) b.push(mk(i));
        assert(b.size() == 4);

        Sample out[4]{};
        assert(b.dump(out, 4) == 4);
        assert(out[0].timestamp_nanos == 1);
        assert(out[3].timestamp_nanos == 4);
    }

    // 4. Wrap-around — older entries overwritten, dump returns newest window.
    {
        HotBuffer b(3);
        for (uint64_t i = 1; i <= 7; ++i) b.push(mk(i));
        assert(b.size() == 3);

        Sample out[3]{};
        assert(b.dump(out, 3) == 3);
        assert(out[0].timestamp_nanos == 5);
        assert(out[1].timestamp_nanos == 6);
        assert(out[2].timestamp_nanos == 7);
    }

    // 5. out_cap smaller than size — truncated to oldest N.
    {
        HotBuffer b(5);
        for (uint64_t i = 1; i <= 5; ++i) b.push(mk(i));

        Sample out[2]{};
        assert(b.dump(out, 2) == 2);
        assert(out[0].timestamp_nanos == 1);
        assert(out[1].timestamp_nanos == 2);
    }

    // 6. Null output / zero capacity — safe no-op.
    {
        HotBuffer b(4);
        b.push(mk(42));
        assert(b.dump(nullptr, 10) == 0);

        Sample out[1]{};
        assert(b.dump(out, 0) == 0);
    }

    // 7. Reset clears state, subsequent dump is empty, new pushes start fresh.
    {
        HotBuffer b(3);
        b.push(mk(1));
        b.push(mk(2));
        b.reset();
        assert(b.size() == 0);

        Sample out[3]{};
        assert(b.dump(out, 3) == 0);

        b.push(mk(100));
        assert(b.size() == 1);
        assert(b.dump(out, 3) == 1);
        assert(out[0].timestamp_nanos == 100);
    }

    // 8. Catch-up invariant — 300-record window fed at 1 Hz.
    {
        HotBuffer b(300);
        for (uint64_t i = 1; i <= 1000; ++i) b.push(mk(i));
        assert(b.size() == 300);

        Sample out[300]{};
        assert(b.dump(out, 300) == 300);
        assert(out[0].timestamp_nanos   == 701);
        assert(out[299].timestamp_nanos == 1000);
    }

    // Capacity 0 is treated as 1 instead of dividing by zero in push().
    {
        HotBuffer hb(0);
        Sample s{};
        s.timestamp_nanos = 7;
        hb.push(s);
        hb.push(s);
        assert(hb.size() == 1);
    }

    // 9. WarmGrace: the hot buffer is emptied once no client has been
    //    connected for the grace period — once per idle spell, never
    //    before the first client, not while a client keeps coming back.
    {
        constexpr uint64_t S = 1000000000ULL;
        WarmGrace w(60 * S);

        // No client yet: nothing pending, never due.
        assert(!w.should_reset(0, 10 * S));
        assert(w.ns_until_due(10 * S) == UINT64_MAX);

        // A client at 100 s, gone from 101 s.
        assert(!w.should_reset(1, 100 * S));
        assert(w.ns_until_due(100 * S) == 60 * S);
        assert(!w.should_reset(0, 101 * S));
        assert(w.ns_until_due(130 * S) == 30 * S);
        assert(!w.should_reset(0, 159 * S));
        assert(w.should_reset(0, 160 * S));               // 60 s after the last client
        assert(!w.should_reset(0, 161 * S));              // once per spell
        assert(w.ns_until_due(161 * S) == UINT64_MAX);

        // A client that comes back within the grace keeps the buffer.
        assert(!w.should_reset(1, 200 * S));
        assert(!w.should_reset(0, 230 * S));
        assert(!w.should_reset(1, 250 * S));              // back again
        assert(!w.should_reset(0, 300 * S));              // 50 s since 250
        assert(w.should_reset(0, 310 * S));

        // A clock that steps back re-anchors instead of expiring.
        assert(!w.should_reset(1, 400 * S));
        assert(!w.should_reset(0, 100 * S));
        assert(!w.should_reset(0, 159 * S));
        assert(w.should_reset(0, 160 * S));

        // Grace 0: emptied on the first tick without a client.
        WarmGrace now(0);
        assert(!now.should_reset(1, 5 * S));
        assert(now.ns_until_due(5 * S) == 0);
        assert(now.should_reset(0, 6 * S));
    }

    std::printf("test_hot_buffer: PASS\n");
    return 0;
}
