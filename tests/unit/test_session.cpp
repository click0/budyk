// SPDX-License-Identifier: BSD-3-Clause
#include "web/session.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace budyk;

static std::string mktmp() {
    char tmpl[] = "/tmp/budyk_sess_XXXXXX";
    int  fd     = ::mkstemp(tmpl);
    assert(fd >= 0);
    ::close(fd);
    return std::string(tmpl);
}

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

    // 6. Persistence — save then load into a fresh store re-validates
    //    the token (the restart-survival path).
    {
        const std::string path = mktmp();
        std::string tok;
        {
            SessionStore a(/*ttl_s*/ 3600);
            tok = a.create();
            assert(!tok.empty());
            assert(a.save(path.c_str()) == 0);
        }
        SessionStore b(/*ttl_s*/ 3600);
        assert(b.load(path.c_str()) == 0);
        assert(b.verify(tok));            // survived the "restart"
        assert(b.size() == 1);
        ::unlink(path.c_str());
    }

    // 7. The saved file is mode 0600 — it holds live bearer tokens.
    {
        const std::string path = mktmp();
        SessionStore a;
        a.create();
        assert(a.save(path.c_str()) == 0);
        struct stat st{};
        assert(::stat(path.c_str(), &st) == 0);
        assert((st.st_mode & 07777) == 0600);
        ::unlink(path.c_str());
    }

    // 8. load() skips entries already past their deadline. Save a store
    //    with TTL 0 (instantly stale), wait, load elsewhere → nothing.
    {
        const std::string path = mktmp();
        {
            SessionStore a(/*ttl_s*/ 0);
            a.create();
            assert(a.save(path.c_str()) == 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        SessionStore b(/*ttl_s*/ 3600);
        assert(b.load(path.c_str()) == 0);
        assert(b.size() == 0);            // expired entry dropped on load
        ::unlink(path.c_str());
    }

    // 9. Autosave — set_persist_path makes create/revoke rewrite the
    //    file, so a sibling store loading it sees the live set.
    {
        const std::string path = mktmp();
        SessionStore a(/*ttl_s*/ 3600);
        a.set_persist_path(path);
        const std::string t1 = a.create();     // autosaved
        const std::string t2 = a.create();     // autosaved
        {
            SessionStore b(/*ttl_s*/ 3600);
            assert(b.load(path.c_str()) == 0);
            assert(b.size() == 2);
            assert(b.verify(t1) && b.verify(t2));
        }
        a.revoke(t1);                           // autosaved
        {
            SessionStore c(/*ttl_s*/ 3600);
            assert(c.load(path.c_str()) == 0);
            assert(c.size() == 1);
            assert(!c.verify(t1));
            assert(c.verify(t2));
        }
        ::unlink(path.c_str());
    }

    // 10. load() on a missing file is a no-op success (fresh install).
    {
        SessionStore a;
        assert(a.load("/tmp/budyk_sess_does_not_exist_zz") == 0);
        assert(a.size() == 0);
    }

    std::printf("test_session: PASS\n");
    return 0;
}
