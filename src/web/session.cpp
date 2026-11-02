// SPDX-License-Identifier: BSD-3-Clause
#include "web/session.h"
#include "core/clock.h"

#include "web/auth.h"
#include "web/sha1_base64.h"

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

constexpr const char kCredentialLine[] = "# credential ";

} // namespace

SessionStore::SessionStore(uint64_t ttl_seconds)
    : ttl_ns_(ttl_seconds * 1000000000ULL) {}

void SessionStore::set_persist_path(const std::string& path) {
    std::lock_guard<std::mutex> g(mtx_);
    persist_path_ = path;
}

void SessionStore::set_credential(const std::string& password_hash) {
    std::lock_guard<std::mutex> g(mtx_);
    if (password_hash.empty()) {
        credential_.clear();
        return;
    }
    // A fingerprint, not the hash: the file is 0600, but it need not
    // hold what argon2 verifies against.
    const std::string digest = sha1(password_hash.data(), password_hash.size());
    credential_ = base64_encode(digest.data(), digest.size());
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
    if (path.empty()) return;
    const int rc = save(path.c_str());
    // Said once per error, not on every login: the result was dropped,
    // and a file that could not be written (a full disk, one left
    // root's by a `sudo budyk serve`) cost every browser its session at
    // the next restart without a word in the log.
    if (rc != last_save_rc_) {
        if (rc != 0) {
            std::fprintf(stderr, "budyk serve: cannot save the sessions to '%s': %s\n",
                         path.c_str(), std::strerror(-rc));
        } else {
            std::fprintf(stderr, "budyk serve: the sessions are saved to '%s' again\n",
                         path.c_str());
        }
        last_save_rc_ = rc;
    }
}

std::string SessionStore::create() {
    const std::string tok = new_session_token();
    if (tok.empty()) return tok;
    const uint64_t deadline = now_realtime_ns() + ttl_ns_;
    {
        std::lock_guard<std::mutex> g(mtx_);
        deadline_[tok] = deadline;
    }
    autosave();
    return tok;
}

bool SessionStore::verify(const std::string& token) {
    if (token.empty()) return false;
    const uint64_t now = now_realtime_ns();
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
            // A deadline past now + TTL: the clock stepped back since it
            // was set, and the session had the step on top of its TTL.
            if (it->second > now + ttl_ns_) it->second = now + ttl_ns_;
        }
    }
    if (evicted) autosave();   // the file should reflect the eviction
    return ok;
}

bool SessionStore::touch(const std::string& token) {
    if (token.empty()) return false;
    const uint64_t now = now_realtime_ns();
    bool evicted = false;
    bool ok      = false;
    bool slid    = false;
    {
        std::lock_guard<std::mutex> g(mtx_);
        auto it = deadline_.find(token);
        if (it == deadline_.end()) return false;
        if (it->second <= now) {
            deadline_.erase(it);
            evicted = true;
        } else {
            ok = true;
            // Forward once an eighth of the TTL has passed; and back down
            // to now + TTL after a clock step back (a session in use gets
            // the whole TTL from now, not the step on top of it).
            const uint64_t fresh = now + ttl_ns_;
            if ((fresh > it->second && fresh - it->second >= ttl_ns_ / 8) ||
                it->second > fresh) {
                it->second = fresh;
                slid = true;
            }
        }
    }
    if (evicted || slid) autosave();
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
    const uint64_t now = now_realtime_ns();
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

int SessionStore::load(const char* path, bool* stale) {
    if (stale != nullptr) *stale = false;
    if (path == nullptr) return -EINVAL;
    std::FILE* f = std::fopen(path, "re");
    if (f == nullptr) return errno == ENOENT ? 0 : -errno;   // none yet: a fresh install

    const uint64_t now = now_realtime_ns();
    char line[256];
    std::string file_credential;
    std::map<std::string, uint64_t> got;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#') {
            if (std::strncmp(line, kCredentialLine, sizeof(kCredentialLine) - 1) == 0) {
                file_credential = line + sizeof(kCredentialLine) - 1;
                while (!file_credential.empty() &&
                       (file_credential.back() == '\n' || file_credential.back() == '\r')) {
                    file_credential.pop_back();
                }
            }
            continue;
        }
        if (line[0] == '\n') continue;
        char* tab = std::strchr(line, '\t');
        if (tab == nullptr) continue;
        *tab = '\0';
        const char* tok_str = line;
        char* endp = nullptr;
        unsigned long long dl = std::strtoull(tab + 1, &endp, 10);
        if (endp == tab + 1)            continue;   // unparsable deadline
        if (tok_str[0] == '\0')         continue;
        if (dl <= now)                  continue;   // already expired
        if (dl > now + ttl_ns_) dl = now + ttl_ns_; // a clock step back: no more than the TTL
        got[std::string(tok_str)] = static_cast<uint64_t>(dl);
    }
    const int read_err = std::ferror(f) ? (errno != 0 ? errno : EIO) : 0;
    std::fclose(f);
    if (read_err != 0) return -read_err;

    std::lock_guard<std::mutex> g(mtx_);
    // Made under another password (or none on record, an older file):
    // not a session of this one. The file is rewritten at the next
    // change, under the credential of now.
    if (!credential_.empty() && file_credential != credential_) {
        if (stale != nullptr) *stale = !got.empty();
        return 0;
    }
    for (const auto& kv : got) deadline_[kv.first] = kv.second;
    return 0;
}

int SessionStore::save(const char* path) const {
    if (path == nullptr) return -EINVAL;

    // One save at a time (save_mtx_), each with the table as it is when
    // its turn comes: the last file written is the newest. The snapshot is
    // taken under mtx_, the I/O done without it.
    std::lock_guard<std::mutex> one(save_mtx_);
    std::map<std::string, uint64_t> snap;
    std::string credential;
    {
        std::lock_guard<std::mutex> g(mtx_);
        snap       = deadline_;
        credential = credential_;
    }

    std::string tmp = path;
    tmp += ".tmp";
    // O_CREAT|0600 so the bearer tokens are never group/other-readable;
    // fchmod afterwards in case umask wasn't honoured on this fs.
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -errno;
    ::fchmod(fd, 0600);
    std::FILE* f = ::fdopen(fd, "w");
    if (f == nullptr) { ::close(fd); ::unlink(tmp.c_str()); return -errno; }

    std::fputs("# budyk sessions v2\n", f);
    if (!credential.empty()) std::fprintf(f, "%s%s\n", kCredentialLine, credential.c_str());
    std::fputs("# token\tdeadline_ns\n", f);
    for (const auto& kv : snap) {
        std::fprintf(f, "%s\t%llu\n", kv.first.c_str(),
                     static_cast<unsigned long long>(kv.second));
    }
    // A file that could not be written whole (a full disk) does not
    // replace the one there; and it reaches the disk before it does (a
    // power cut after a login otherwise left an empty file under the
    // name, on a filesystem that writes the rename first).
    const bool flushed = std::fflush(f) == 0 && std::ferror(f) == 0 && ::fsync(fd) == 0;
    const int  werr    = flushed ? 0 : errno;
    if (std::fclose(f) != 0 || !flushed) {
        const int e = werr != 0 ? werr : errno;
        ::unlink(tmp.c_str());
        return -(e != 0 ? e : EIO);
    }

    if (std::rename(tmp.c_str(), path) != 0) {
        const int e = -errno;
        std::remove(tmp.c_str());
        return e;
    }
    return 0;
}

} // namespace budyk
