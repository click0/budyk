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

    std::printf("test_hot_buffer: PASS\n");
    return 0;
}
