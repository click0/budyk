// SPDX-License-Identifier: BSD-3-Clause
// Cross-platform file watcher exercise — uses real I/O against temp
// files and asserts that Modified / Deleted events arrive promptly.
// Runs on Linux (inotify) and FreeBSD (kqueue); the stub backend on
// other platforms is skipped.

#include "security/file_watcher.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace budyk;

#if defined(BUDYK_LINUX) || defined(BUDYK_FREEBSD)

// Create a temp file with a single byte of content and return its path.
// Caller is responsible for unlink()ing.
static std::string make_tmp() {
    char tmpl[] = "/tmp/budyk_fw_XXXXXX";
    int  fd     = ::mkstemp(tmpl);
    assert(fd >= 0);
    const char z = 'a';
    [[maybe_unused]] ssize_t w = ::write(fd, &z, 1);
    ::close(fd);
    return std::string(tmpl);
}

// Whether `events` has (path, kind).
static bool has(const std::vector<FileChangeEvent>& events, const std::string& path,
                FileChangeKind kind) {
    for (const auto& ev : events) {
        if (ev.path == path && ev.kind == kind) return true;
    }
    return false;
}

// Poll until (path, kind) arrives or 2 s pass; the events seen go in *all.
static bool wait_for(FileWatcher& fw, const std::string& path, FileChangeKind kind,
                     std::vector<FileChangeEvent>* all) {
    for (int i = 0; i < 20 && !has(*all, path, kind); ++i) fw.poll(100, all);
    return has(*all, path, kind);
}

static void touch(const std::string& path, const char* content) {
    std::FILE* f = std::fopen(path.c_str(), "a");
    assert(f != nullptr);
    std::fputs(content, f);
    std::fclose(f);
}

int main() {
    // 1. init + add + count round-trip.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p = make_tmp();
        const int id = fw.add(p);
        assert(id > 0);
        assert(fw.count() == 1);
        fw.remove(p);
        assert(fw.count() == 0);
        fw.shutdown();
        ::unlink(p.c_str());
    }

    // 2. Modify a watched file → Modified event arrives.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p = make_tmp();
        assert(fw.add(p) > 0);

        // Modify after the watch is in place.
        touch(p, "hello");

        std::vector<FileChangeEvent> events;
        const int n = fw.poll(2000, &events);
        assert(n > 0);
        bool saw_modified = false;
        for (const auto& ev : events) {
            if (ev.path == p && ev.kind == FileChangeKind::Modified) {
                saw_modified = true; break;
            }
        }
        assert(saw_modified);

        fw.shutdown();
        ::unlink(p.c_str());
    }

    // 3. Coalescing — many writes inside one poll window collapse to a
    //    single (path, Modified) event.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p = make_tmp();
        assert(fw.add(p) > 0);

        for (int i = 0; i < 10; ++i) touch(p, "x");

        std::vector<FileChangeEvent> events;
        const int n = fw.poll(2000, &events);
        assert(n >= 1);

        int modified_count = 0;
        for (const auto& ev : events) {
            if (ev.path == p && ev.kind == FileChangeKind::Modified) {
                ++modified_count;
            }
        }
        assert(modified_count == 1);

        fw.shutdown();
        ::unlink(p.c_str());
    }

    // 4. Delete the file → Deleted event arrives.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p = make_tmp();
        assert(fw.add(p) > 0);

        ::unlink(p.c_str());

        std::vector<FileChangeEvent> events;
        const int n = fw.poll(2000, &events);
        assert(n > 0);
        bool saw_deleted = false;
        for (const auto& ev : events) {
            if (ev.path == p && ev.kind == FileChangeKind::Deleted) {
                saw_deleted = true; break;
            }
        }
        assert(saw_deleted);

        fw.shutdown();
    }

    // 5. Polling with timeout 0 returns immediately with no events
    //    when nothing has happened.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p = make_tmp();
        assert(fw.add(p) > 0);

        std::vector<FileChangeEvent> events;
        const int n = fw.poll(0, &events);
        assert(n == 0);
        assert(events.empty());

        fw.shutdown();
        ::unlink(p.c_str());
    }

    // 6. add() on a non-existent path fails with -errno.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const int rc = fw.add("/tmp/budyk_fw_does_not_exist_zzz");
        assert(rc < 0);
        assert(fw.count() == 0);
        fw.shutdown();
    }

    // 7. shutdown() is idempotent + safe to call before init().
    {
        FileWatcher fw;
        fw.shutdown();   // no-op pre-init
        assert(fw.init() == 0);
        fw.shutdown();
        fw.shutdown();   // double-shutdown is safe too
    }

    // 8. A save by rename (vim, visudo, sed -i: write a new file, rename
    //    it over the old) is Deleted then Created, and the path is
    //    watched again: the next write is seen. The watch used to stay on
    //    the old inode, gone, and nothing after the first save showed up.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p   = make_tmp();
        const std::string tmp = p + ".new";
        assert(fw.add(p) > 0);
        for (int round = 0; round < 3; ++round) {
            touch(tmp, "replaced");
            assert(::rename(tmp.c_str(), p.c_str()) == 0);
            std::vector<FileChangeEvent> events;
            assert(wait_for(fw, p, FileChangeKind::Deleted, &events));
            assert(wait_for(fw, p, FileChangeKind::Created, &events));
            assert(fw.count() == 1);

            touch(p, "then edited in place");
            events.clear();
            assert(wait_for(fw, p, FileChangeKind::Modified, &events));
        }
        fw.shutdown();
        ::unlink(p.c_str());
    }

    // 9. rm, then the file comes back later: not watched (count 0) in
    //    between, Created and watched again once it is there.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p = make_tmp();
        assert(fw.add(p) > 0);
        ::unlink(p.c_str());
        std::vector<FileChangeEvent> events;
        assert(wait_for(fw, p, FileChangeKind::Deleted, &events));
        assert(fw.count() == 0);
        events.clear();
        assert(fw.poll(0, &events) == 0);

        touch(p, "back");
        assert(wait_for(fw, p, FileChangeKind::Created, &events));
        assert(fw.count() == 1);
        events.clear();
        touch(p, "and changed");
        assert(wait_for(fw, p, FileChangeKind::Modified, &events));
        fw.shutdown();
        ::unlink(p.c_str());
    }

    // 10. Two paths to one inode (a hard link): a write is reported for
    //     both (inotify gives both the same wd, and the second path used
    //     to take it over), and removing one keeps the other watched.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p = make_tmp();
        const std::string q = p + ".link";
        assert(::link(p.c_str(), q.c_str()) == 0);
        assert(fw.add(p) > 0);
        assert(fw.add(q) > 0);
        assert(fw.count() == 2);
        touch(p, "x");
        std::vector<FileChangeEvent> events;
        assert(wait_for(fw, p, FileChangeKind::Modified, &events));
        assert(wait_for(fw, q, FileChangeKind::Modified, &events));

        fw.remove(q);
        assert(fw.count() == 1);
        events.clear();
        touch(q, "y");                          // the same file, through the other name
        assert(wait_for(fw, p, FileChangeKind::Modified, &events));
        assert(!has(events, q, FileChangeKind::Modified));
        fw.shutdown();
        ::unlink(p.c_str());
        ::unlink(q.c_str());
    }

    // 11. The watched file moved away: Deleted for its path, and writes to
    //     it where it went are not reported as the path's.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p     = make_tmp();
        const std::string moved = p + ".moved";
        assert(fw.add(p) > 0);
        assert(::rename(p.c_str(), moved.c_str()) == 0);
        std::vector<FileChangeEvent> events;
        assert(wait_for(fw, p, FileChangeKind::Deleted, &events));
        events.clear();
        touch(moved, "elsewhere");
        fw.poll(300, &events);
        assert(!has(events, p, FileChangeKind::Modified));
        assert(fw.count() == 0);
        fw.shutdown();
        ::unlink(moved.c_str());
    }

    // 11b. A save by rename while the old file lives on: a reader holds it
    //      open, and it has another name. inotify gives the old file only
    //      IN_ATTRIB then, and IN_DELETE_SELF once the last of those goes,
    //      or never: the path stayed watched on the old file, and writes
    //      to the new one were missed (vipw or a package upgrade while
    //      anyone read /etc/passwd). Deleted then Created, and the next
    //      write is seen. The old file's other name removed is Deleted for
    //      it, the reader still there.
    {
        FileWatcher fw;
        assert(fw.init() == 0);
        const std::string p    = make_tmp();
        const std::string keep = p + ".keep";
        assert(::link(p.c_str(), keep.c_str()) == 0);
        const int held = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        assert(held >= 0);
        assert(fw.add(p) > 0);
        touch(p + ".new", "replaced");
        assert(::rename((p + ".new").c_str(), p.c_str()) == 0);
        std::vector<FileChangeEvent> events;
        assert(wait_for(fw, p, FileChangeKind::Deleted, &events));
        assert(wait_for(fw, p, FileChangeKind::Created, &events));
        events.clear();
        touch(p, "edited after the save");
        assert(wait_for(fw, p, FileChangeKind::Modified, &events));

        assert(fw.add(keep) > 0);
        assert(fw.count() == 2);
        events.clear();
        assert(::unlink(keep.c_str()) == 0);
        assert(wait_for(fw, keep, FileChangeKind::Deleted, &events));
        assert(fw.count() == 1);
        ::close(held);
        fw.shutdown();
        ::unlink(p.c_str());
    }

#if defined(BUDYK_LINUX)
    // 12. inotify's queue overflows (more events than
    //     fs.inotify.max_queued_events between two polls; alternating
    //     kinds, which the kernel does not merge): every path is reported,
    //     and a path whose file was replaced meanwhile (its events lost
    //     in the overflow) is watched again, not left on the old inode.
    {
        long max_events = 0;
        if (std::FILE* f = std::fopen("/proc/sys/fs/inotify/max_queued_events", "r")) {
            if (std::fscanf(f, "%ld", &max_events) != 1) max_events = 0;
            std::fclose(f);
        }
        if (max_events > 0 && max_events <= 200000) {
            FileWatcher fw;
            assert(fw.init() == 0);
            const std::string a = make_tmp();
            const std::string b = make_tmp();
            assert(fw.add(a) > 0);
            assert(fw.add(b) > 0);
            for (long i = 0; i <= max_events; ++i) {             // IN_ATTRIB, IN_MODIFY, ...
                assert(::chmod(a.c_str(), (i & 1) != 0 ? 0600 : 0640) == 0);
                touch(a, "x");
            }
            touch(b + ".new", "replaced");
            assert(::rename((b + ".new").c_str(), b.c_str()) == 0);   // lost in the overflow
            std::vector<FileChangeEvent> events;
            fw.poll(1000, &events);
            assert(has(events, a, FileChangeKind::Modified));
            assert(has(events, b, FileChangeKind::Deleted) || has(events, b, FileChangeKind::Modified));
            assert(fw.count() == 2);
            events.clear();
            fw.poll(0, &events);                                  // drain
            events.clear();
            touch(b, "changed after");
            assert(wait_for(fw, b, FileChangeKind::Modified, &events));
            fw.shutdown();
            ::unlink(a.c_str());
            ::unlink(b.c_str());
        }
    }
#endif

    std::printf("test_file_watcher: PASS\n");
    return 0;
}

#else
int main() {
    // No backend on this platform — emit PASS so ctest doesn't flag
    // the build as broken. The stub FileWatcher returns -ENOSYS at
    // runtime and we have no way to exercise it positively here.
    std::printf("test_file_watcher: SKIP (no inotify/kqueue)\n");
    return 0;
}
#endif
