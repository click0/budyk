// SPDX-License-Identifier: BSD-3-Clause
// parse_int_full: the strict parser behind --port, --timeout and the
// YAML rules' for_ticks / cooldown.

#include "core/parse_int.h"

#include <cassert>
#include <climits>
#include <cstdio>

using namespace budyk;

int main() {
    long v = 0;

    // 1. Plain values, signs, the bounds themselves.
    assert(parse_int_full("8080", 1, 65535, &v) && v == 8080);
    assert(parse_int_full("-1", -1, INT_MAX, &v) && v == -1);
    assert(parse_int_full("+7", 0, 10, &v) && v == 7);
    assert(parse_int_full("0", 0, 0, &v) && v == 0);
    assert(parse_int_full("65535", 1, 65535, &v) && v == 65535);

    // 2. What atoi() would have read as a number: rejected, *out kept.
    v = 42;
    const char* bad[] = { "", " 5", "5 ", "12abc", "abc", "0x10", "-", "+",
                          "1.5", "30s", "--1", "\t3" };
    for (const char* s : bad) {
        assert(!parse_int_full(s, LONG_MIN, LONG_MAX, &v));
        assert(v == 42);
    }
    assert(!parse_int_full(nullptr, 0, 1, &v));

    // 3. Out of range, including past what a long holds.
    assert(!parse_int_full("0", 1, 65535, &v));
    assert(!parse_int_full("65536", 1, 65535, &v));
    assert(!parse_int_full("-2", -1, INT_MAX, &v));
    assert(!parse_int_full("99999999999999999999999", LONG_MIN, LONG_MAX, &v));
    assert(!parse_int_full("-99999999999999999999999", LONG_MIN, LONG_MAX, &v));
    assert(v == 42);

    std::printf("test_parse_int: PASS\n");
    return 0;
}
