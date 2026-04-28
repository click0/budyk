// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace budyk {

// Process-local session table. Each token has a TTL; verify() refuses
// expired tokens and lazily evicts them. The store is thread-safe —
// HttpServer's worker calls into it concurrently with the collector.
//
// Tokens are 32-byte hex strings produced by web::auth::new_session_token().
class SessionStore {
public:
    explicit SessionStore(uint64_t ttl_seconds = 86400);   // 24 h default

    // Mint a fresh token, persist it with the configured TTL, return it.
    // Empty string on entropy-source failure.
    std::string create();

    // True when `token` is present and not yet expired.
    bool        verify(const std::string& token);

    // Drop a token if present.
    void        revoke(const std::string& token);

    // Removes every entry whose deadline has passed; returns count
    // purged. Called opportunistically by verify() / create().
    size_t      purge_expired();

    size_t      size() const;

private:
    mutable std::mutex mtx_;
    uint64_t           ttl_ns_;
    std::map<std::string, uint64_t> deadline_;   // token → expires_at_ns
};

} // namespace budyk
