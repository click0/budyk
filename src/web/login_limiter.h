// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace budyk {

// Throttle for /api/auth/login. A password check is an Argon2id run
// (64 MiB, tens of milliseconds) on the single HTTP thread, so an
// unlimited stream of attempts is both a brute-force channel and a
// cheap way to keep everyone else out. The limiter counts failed
// attempts per client address in a fixed window: once a client has
// `max_failures` failures in `window_sec`, its attempts are refused
// (HTTP 429, no hash computed) until the window ends. A second,
// address-independent window with `global_max` failures catches many
// addresses taking turns, and a reverse proxy that makes every client
// look like one address. A successful login clears that address.
//
// Not thread-safe: the HTTP server calls it from its one I/O thread.
class LoginLimiter {
public:
    LoginLimiter(int max_failures = 5, int window_sec = 60, int global_max = 50);

    // Seconds the client must wait before an attempt will be looked at;
    // 0 means attempt now. Also sweeps expired entries.
    int    retry_after(const std::string& peer, uint64_t now_sec);
    // Returns true when this failure is the one that blocks the peer,
    // so the caller can log it once.
    bool   record_failure(const std::string& peer, uint64_t now_sec);
    void   record_success(const std::string& peer);

    size_t tracked() const;             // addresses with a live window
    int    max_failures() const;
    int    window_sec()   const;

private:
    struct Entry {
        int      failures     = 0;
        uint64_t window_start = 0;
    };
    bool expired(const Entry& e, uint64_t now) const;
    void sweep(uint64_t now);

    int      max_failures_;
    int      window_sec_;
    int      global_max_;
    std::unordered_map<std::string, Entry> peers_;
    Entry    global_;
};

} // namespace budyk
