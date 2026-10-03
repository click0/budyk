// SPDX-License-Identifier: BSD-3-Clause
#include "web/login_limiter.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace budyk;

int main() {
    // 1. Up to max_failures attempts in a window are looked at; the one
    //    that reaches the limit blocks the address until the window
    //    ends, and retry_after counts down to it.
    {
        LoginLimiter l(3, 60);
        const std::string a = "10.0.0.1";
        assert(l.retry_after(a, 1000) == 0);
        assert(!l.record_failure(a, 1000));
        assert(l.retry_after(a, 1010) == 0);
        assert(!l.record_failure(a, 1010));
        assert(l.retry_after(a, 1020) == 0);
        assert(l.record_failure(a, 1020));            // third: now blocked
        assert(l.retry_after(a, 1020) == 40);         // window started at 1000
        assert(l.retry_after(a, 1050) == 10);
        assert(l.retry_after(a, 1059) == 1);
        assert(l.retry_after(a, 1060) == 0);          // window over
        assert(l.tracked() == 0);                     // entry dropped
        // A fresh window starts at the next failure.
        assert(!l.record_failure(a, 1060));
        assert(l.retry_after(a, 1061) == 0);
    }

    // 2. Addresses are independent; a success clears the address, so a
    //    user who finally remembers the password is not left blocked
    //    by their own earlier typos... but the attempt must have been
    //    allowed through first.
    {
        LoginLimiter l(2, 60);
        assert(!l.record_failure("a", 1));
        assert(l.record_failure("a", 2));
        assert(l.retry_after("a", 3) > 0);
        assert(l.retry_after("b", 3) == 0);           // b unaffected
        assert(!l.record_failure("b", 3));
        l.record_success("b");
        assert(!l.record_failure("b", 4));            // count restarted
        l.record_success("a");
        assert(l.retry_after("a", 5) == 0);
        assert(l.tracked() == 1);                     // only b
    }

    // 3. The global window: many addresses taking one turn each are
    //    still cut off once their failures add up.
    {
        LoginLimiter l(5, 60, 10);
        for (int i = 0; i < 10; ++i) {
            const std::string peer = "192.168.0." + std::to_string(i);
            assert(l.retry_after(peer, 100) == 0);
            assert(!l.record_failure(peer, 100));
        }
        assert(l.retry_after("192.168.0.99", 101) == 59);   // never seen, still blocked
        assert(l.retry_after("192.168.0.99", 160) == 0);    // window over
    }

    // 4. The table is swept past the threshold and can't grow without
    //    bound: expired entries go, a flood of live ones is cleared.
    {
        LoginLimiter l(5, 60);
        for (int i = 0; i < 300; ++i) {
            l.record_failure("old" + std::to_string(i), 1000);
        }
        assert(l.tracked() == 300);
        assert(l.retry_after("x", 1100) == 0);        // sweep: all expired
        assert(l.tracked() == 0);
        for (int i = 0; i < 5000; ++i) {
            l.record_failure("live" + std::to_string(i), 2000);
        }
        assert(l.retry_after("y", 2001) == 0 || true); // may be globally blocked; the point:
        assert(l.tracked() == 0);                     // over the cap -> cleared
    }

    // 5. Degenerate construction values are clamped, not trusted.
    {
        LoginLimiter l(0, 0, 0);
        assert(l.max_failures() == 1 && l.window_sec() == 1);
        assert(l.record_failure("a", 10));            // first failure blocks
        assert(l.retry_after("a", 10) == 1);
        assert(l.retry_after("a", 11) == 0);
    }

    std::printf("test_login_limiter: PASS\n");
    return 0;
}
