// SPDX-License-Identifier: BSD-3-Clause
#include "web/session.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace budyk;

int main() {
    // 1. Round-trip — token minted, verifies, then revoke removes it.
    {
        SessionStore store(/*ttl_s*/ 60);
        const std::string t = store.create();
        assert(!t.empty());
        assert(t.size() == 64);            // 32 random bytes hex-encoded
        assert(store.verify(t));
        assert(store.size() == 1);
        store.revoke(t);
        assert(!store.verify(t));
        assert(store.size() == 0);
    }

    // 2. Empty / unknown token rejected.
    {
        SessionStore store;
        assert(!store.verify(""));
        assert(!store.verify("not-a-real-token-deadbeef"));
    }

    // 3. Two tokens are distinct and verify independently.
    {
        SessionStore store;
        const std::string a = store.create();
        const std::string b = store.create();
        assert(a != b);
        assert(store.verify(a));
        assert(store.verify(b));
        store.revoke(a);
        assert(!store.verify(a));
        assert(store.verify(b));         // independent
    }

    // 4. Expired token — TTL of 0 makes every token immediately stale,
    //    so verify() refuses on the next call. Also exercises lazy-evict.
    {
        SessionStore store(/*ttl_s*/ 0);
        const std::string t = store.create();
        assert(!t.empty());
        // Sleep 5 ms to make the deadline definitively in the past.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(!store.verify(t));
        assert(store.size() == 0);       // evicted on the failing verify
    }

    // 5. purge_expired removes only stale entries and counts them.
    {
        SessionStore store(/*ttl_s*/ 0);
        store.create();
        store.create();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        SessionStore fresh(/*ttl_s*/ 60);
        const std::string keep = fresh.create();
        assert(store.purge_expired() == 2);
        assert(store.size() == 0);
        assert(fresh.verify(keep));
    }

    std::printf("test_session: PASS\n");
    return 0;
}
