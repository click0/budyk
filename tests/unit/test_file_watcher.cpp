// SPDX-License-Identifier: BSD-3-Clause
// Cross-platform file watcher exercise — uses real I/O against temp
// files and asserts that Modified / Deleted events arrive promptly.
// Runs on Linux (inotify) and FreeBSD (kqueue); the stub backend on
// other platforms is skipped.

#include "security/file_watcher.h"

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
