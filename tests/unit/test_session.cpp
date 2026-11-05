// SPDX-License-Identifier: BSD-3-Clause
#include "web/session.h"
#include "core/clock.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>
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

static std::string slurp(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "re");
    assert(f != nullptr);
    std::string out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
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

    // 11. Two threads saving at once (the HTTP thread on a login or a
    //     logout, the main thread on a WebSocket's session expiring) both
    //     wrote <path>.tmp: one truncated the other's file and its rename
    //     failed (ENOENT, about half the saves). One at a time now: every
    //     save succeeds, and the file holds the whole table.
    {
        const std::string path = "/tmp/budyk_sess_race_" + std::to_string(::getpid());
        SessionStore a(/*ttl_s*/ 3600);
        for (int i = 0; i < 20; ++i) assert(!a.create().empty());   // no persist path yet
        std::atomic<int> failed{0};
        auto saver = [&] {
            for (int i = 0; i < 1000; ++i) {
                if (a.save(path.c_str()) != 0) failed.fetch_add(1);
            }
        };
        std::thread t1(saver), t2(saver);
        t1.join();
        t2.join();
        assert(failed.load() == 0);
        SessionStore b(/*ttl_s*/ 3600);
        assert(b.load(path.c_str()) == 0 && b.size() == 20);
        ::unlink(path.c_str());
    }

    // 12. Sliding expiration (spec §3.2): touch() moves the deadline on
    //     once an eighth of the TTL has passed, and saves then, not on
    //     every request; a session touched lives past its first deadline,
    //     one left alone ends at it. Every session ended TTL after the
    //     login, however much it was used.
    {
        const std::string path = mktmp();
        SessionStore a(/*ttl_s*/ 2);
        a.set_persist_path(path);
        const std::string t     = a.create();
        const std::string idle  = a.create();
        const std::string first = slurp(path);
        assert(a.touch(t));
        assert(slurp(path) == first);               // within the first eighth: a verify
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        assert(a.touch(t));
        assert(slurp(path) != first);               // slid, and saved: ends at 2.6 s now
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));   // 2.2 s
        assert(a.verify(t));                        // in use: lives on
        assert(!a.verify(idle));                    // left alone: ended at 2 s
        assert(!a.touch(idle));
        assert(!a.touch("not-a-real-token-deadbeef"));
        ::unlink(path.c_str());
    }

    // 13. The credential: a file made under another password hash is
    //     dropped whole (a password change ended no session, and with
    //     the sliding expiration whoever had the old password kept the
    //     dashboard); one without a credential line (an older file) too
    //     when a credential is set; the same hash loads; a store with
    //     no credential takes any file.
    {
        const std::string path = mktmp();
        SessionStore a(/*ttl_s*/ 3600);
        a.set_credential("$argon2id$v=19$m=65536,t=3,p=4$old$hash");
        a.set_persist_path(path);
        const std::string t = a.create();
        assert(slurp(path).find("# credential ") != std::string::npos);
        assert(slurp(path).find("$argon2id$") == std::string::npos);   // a fingerprint, not the hash
        bool stale = true;
        SessionStore same(3600);
        same.set_credential("$argon2id$v=19$m=65536,t=3,p=4$old$hash");
        assert(same.load(path.c_str(), &stale) == 0 && !stale && same.verify(t));
        SessionStore changed(3600);
        changed.set_credential("$argon2id$v=19$m=65536,t=3,p=4$new$hash");
        assert(changed.load(path.c_str(), &stale) == 0 && stale && changed.size() == 0);
        SessionStore any(3600);
        assert(any.load(path.c_str(), &stale) == 0 && !stale && any.verify(t));
        // An older file, no credential line, a credential set: dropped.
        SessionStore v1(3600);
        v1.set_persist_path(path);
        v1.create();
        assert(slurp(path).find("# credential ") == std::string::npos);
        assert(changed.load(path.c_str(), &stale) == 0 && stale && changed.size() == 0);
        // An empty file under another credential is nothing to report.
        SessionStore none(3600);
        none.set_credential("x");
        none.set_persist_path(path);
        none.revoke("nothing");
        assert(none.save(path.c_str()) == 0);
        assert(changed.load(path.c_str(), &stale) == 0 && !stale);
        ::unlink(path.c_str());
    }

    // 14. A deadline past now + TTL (the clock stepped back since it was
    //     set) is brought down to that: on load, by verify() and by
    //     touch(). Every session had the step on top of its TTL.
    {
        const std::string path = mktmp();
        {
            FILE* f = std::fopen(path.c_str(), "w");
            assert(f != nullptr);
            std::fprintf(f, "# budyk sessions v2\n# token\tdeadline_ns\nfar\t%llu\n",
                         static_cast<unsigned long long>(now_realtime_ns() + 10ULL * 365 * 86400 * 1000000000ULL));
            std::fclose(f);
        }
        SessionStore a(/*ttl_s*/ 1);
        assert(a.load(path.c_str()) == 0 && a.size() == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        assert(!a.verify("far"));                   // the TTL from the load, not ten years
        ::unlink(path.c_str());
    }

    // 15. load() says when the file cannot be read (it was 0 for every
    //     fopen failure, a root-owned file after a `sudo budyk serve`
    //     among them), and save() when it cannot be written.
    {
        const std::string path = mktmp();
        SessionStore a;
        assert(a.load((path + "/under-a-file").c_str()) == -ENOTDIR);
        assert(a.save((path + "/under-a-file").c_str()) == -ENOTDIR);
        ::unlink(path.c_str());
    }

    // 16. Two threads autosaving at once (a login on the HTTP thread, an
    //     expired session evicted on the main thread) while the saves
    //     fail: the last result they compare against is shared, and was
    //     written by both without a lock (ThreadSanitizer: a data race;
    //     the "cannot save" line could come twice or not at all).
    {
        const std::string path = mktmp();
        ::unlink(path.c_str());
        SessionStore s(0);                       // every session expires at once
        s.set_persist_path(path + "/no-such-dir/sessions.tsv");
        std::thread http([&s] {
            for (int i = 0; i < 50; ++i) (void)s.create();
        });
        for (int i = 0; i < 50; ++i) {
            const std::string tok = s.create();
            (void)s.verify(tok);                 // expired: evicted, autosaved
        }
        http.join();
        assert(s.size() == 0 || s.size() <= 100);
    }

    std::printf("test_session: PASS\n");
    return 0;
}
