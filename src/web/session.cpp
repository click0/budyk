// SPDX-License-Identifier: BSD-3-Clause
#include "web/session.h"

#include "web/auth.h"

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

std::string SessionStore::create() {
    const std::string tok = new_session_token();
    if (tok.empty()) return tok;
    const uint64_t deadline = now_ns() + ttl_ns_;
    {
        std::lock_guard<std::mutex> g(mtx_);
        deadline_[tok] = deadline;
    }
    return tok;
}

bool SessionStore::verify(const std::string& token) {
    if (token.empty()) return false;
    const uint64_t now = now_ns();
    std::lock_guard<std::mutex> g(mtx_);
    auto it = deadline_.find(token);
    if (it == deadline_.end()) return false;
    if (it->second <= now) {
        deadline_.erase(it);
        return false;
    }
    return true;
}

void SessionStore::revoke(const std::string& token) {
    std::lock_guard<std::mutex> g(mtx_);
    deadline_.erase(token);
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

} // namespace budyk
