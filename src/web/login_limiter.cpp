// SPDX-License-Identifier: BSD-3-Clause
#include "web/login_limiter.h"

namespace budyk {

namespace {
// The table is swept when it grows past this many addresses, and
// cleared outright if a sweep leaves more than kMaxTracked: a flood
// from many addresses must not grow memory without bound. Clearing
// forgets some failure counts, which only ever errs towards allowing
// an attempt that then costs one hash.
constexpr size_t kSweepAt    = 256;
constexpr size_t kMaxTracked = 4096;
} // namespace

LoginLimiter::LoginLimiter(int max_failures, int window_sec, int global_max)
    : max_failures_(max_failures > 0 ? max_failures : 1),
      window_sec_(window_sec > 0 ? window_sec : 1),
      global_max_(global_max > 0 ? global_max : 1) {}

bool LoginLimiter::expired(const Entry& e, uint64_t now) const {
    return e.failures == 0 ||
           now >= e.window_start + static_cast<uint64_t>(window_sec_);
}

void LoginLimiter::sweep(uint64_t now) {
    if (peers_.size() < kSweepAt) return;
    for (auto it = peers_.begin(); it != peers_.end(); ) {
        if (expired(it->second, now)) it = peers_.erase(it);
        else                          ++it;
    }
    if (peers_.size() > kMaxTracked) peers_.clear();
}

int LoginLimiter::retry_after(const std::string& peer, uint64_t now_sec) {
    sweep(now_sec);
    const Entry* blocking = nullptr;
    if (!expired(global_, now_sec) && global_.failures >= global_max_) {
        blocking = &global_;
    }
    auto it = peers_.find(peer);
    if (it != peers_.end()) {
        if (expired(it->second, now_sec)) {
            peers_.erase(it);
        } else if (it->second.failures >= max_failures_) {
            const Entry& e = it->second;
            if (blocking == nullptr || e.window_start > blocking->window_start) blocking = &e;
        }
    }
    if (blocking == nullptr) return 0;
    const uint64_t until = blocking->window_start + static_cast<uint64_t>(window_sec_);
    return until > now_sec ? static_cast<int>(until - now_sec) : 1;
}

bool LoginLimiter::record_failure(const std::string& peer, uint64_t now_sec) {
    auto bump = [&](Entry* e) {
        if (expired(*e, now_sec)) {
            e->failures     = 0;
            e->window_start = now_sec;
        }
        ++e->failures;
    };
    bump(&global_);
    Entry& e = peers_[peer];
    bump(&e);
    return e.failures == max_failures_;
}

void LoginLimiter::record_success(const std::string& peer) {
    peers_.erase(peer);
}

size_t LoginLimiter::tracked() const { return peers_.size(); }
int    LoginLimiter::max_failures() const { return max_failures_; }
int    LoginLimiter::window_sec()   const { return window_sec_; }

} // namespace budyk
