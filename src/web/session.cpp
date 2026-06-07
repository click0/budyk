// SPDX-License-Identifier: BSD-3-Clause
#include "web/session.h"

#include "web/auth.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace budyk {

namespace {

uint64_t now_ns() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace

SessionStore::SessionStore(uint64_t ttl_seconds)
    : ttl_ns_(ttl_seconds * 1000000000ULL) {}

void SessionStore::set_persist_path(const std::string& path) {
    std::lock_guard<std::mutex> g(mtx_);
    persist_path_ = path;
}

void SessionStore::autosave() const {
    // Snapshot the path under the lock; the actual write re-locks
    // inside save() to copy the table, so no file I/O happens while a
    // caller holds mtx_.
    std::string path;
    {
        std::lock_guard<std::mutex> g(mtx_);
        path = persist_path_;
    }
    if (!path.empty()) save(path.c_str());
}

std::string SessionStore::create() {
    const std::string tok = new_session_token();
    if (tok.empty()) return tok;
    const uint64_t deadline = now_ns() + ttl_ns_;
    {
        std::lock_guard<std::mutex> g(mtx_);
        deadline_[tok] = deadline;
    }
    autosave();
    return tok;
}

bool SessionStore::verify(const std::string& token) {
    if (token.empty()) return false;
    const uint64_t now = now_ns();
    bool evicted = false;
    bool ok      = false;
    {
        std::lock_guard<std::mutex> g(mtx_);
        auto it = deadline_.find(token);
        if (it == deadline_.end()) return false;
        if (it->second <= now) {
            deadline_.erase(it);
            evicted = true;
        } else {
            ok = true;
        }
    }
    if (evicted) autosave();   // the file should reflect the eviction
    return ok;
}

void SessionStore::revoke(const std::string& token) {
    bool removed;
    {
        std::lock_guard<std::mutex> g(mtx_);
        removed = deadline_.erase(token) > 0;
    }
    if (removed) autosave();
}

size_t SessionStore::purge_expired() {
    const uint64_t now = now_ns();
    std::lock_guard<std::mutex> g(mtx_);
    size_t purged = 0;
    for (auto it = deadline_.begin(); it != deadline_.end(); ) {
        if (it->second <= now) { it = deadline_.erase(it); ++purged; }
        else                   { ++it; }
    }
    return purged;
}

size_t SessionStore::size() const {
    std::lock_guard<std::mutex> g(mtx_);
    return deadline_.size();
}

int SessionStore::load(const char* path) {
    if (path == nullptr) return -EINVAL;
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return 0;   // no sessions yet — fresh install, fine

    const uint64_t now = now_ns();
    char line[256];
    std::lock_guard<std::mutex> g(mtx_);
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char* tab = std::strchr(line, '\t');
        if (tab == nullptr) continue;
        *tab = '\0';
        const char* tok_str = line;
        char* endp = nullptr;
        const unsigned long long dl = std::strtoull(tab + 1, &endp, 10);
        if (endp == tab + 1)            continue;   // unparsable deadline
        if (tok_str[0] == '\0')         continue;
        if (dl <= now)                  continue;   // already expired
        deadline_[std::string(tok_str)] = static_cast<uint64_t>(dl);
    }
    std::fclose(f);
    return 0;
}

int SessionStore::save(const char* path) const {
    if (path == nullptr) return -EINVAL;

    // Snapshot under the lock, then do I/O unlocked.
    std::map<std::string, uint64_t> snap;
    {
        std::lock_guard<std::mutex> g(mtx_);
        snap = deadline_;
    }

    std::string tmp = path;
    tmp += ".tmp";
    // O_CREAT|0600 so the bearer tokens are never group/other-readable;
    // fchmod afterwards in case umask wasn't honoured on this fs.
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -errno;
    ::fchmod(fd, 0600);
    std::FILE* f = ::fdopen(fd, "w");
    if (f == nullptr) { ::close(fd); ::unlink(tmp.c_str()); return -errno; }

    std::fputs("# budyk sessions v1\n", f);
    std::fputs("# token\tdeadline_ns\n", f);
    for (const auto& kv : snap) {
        std::fprintf(f, "%s\t%llu\n", kv.first.c_str(),
                     static_cast<unsigned long long>(kv.second));
    }
    std::fflush(f);
    std::fclose(f);

    if (std::rename(tmp.c_str(), path) != 0) {
        const int e = -errno;
        std::remove(tmp.c_str());
        return e;
    }
    return 0;
}

} // namespace budyk
