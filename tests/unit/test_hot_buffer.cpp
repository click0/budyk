// SPDX-License-Identifier: BSD-3-Clause
#include "hot_buffer/hot_buffer.h"
#include <cassert>
#include <cstdio>

int main() {
    budyk::HotBuffer buf(10);
    assert(buf.size() == 0);

    budyk::Sample s{};
    s.timestamp_nanos = 1;
    buf.push(s);
    assert(buf.size() == 1);

    buf.reset();
    assert(buf.size() == 0);

    std::printf("test_hot_buffer: PASS\n");
    return 0;
}
