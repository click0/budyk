// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <string>

namespace budyk {

// Argon2id password hashing + verification (spec §3.2, §5.9).
// Wraps libargon2's reference implementation. Defaults follow OWASP
// 2024 interactive-login recommendations: t=3, m=64 MiB, p=4.
struct Argon2Params {
    unsigned t_cost     = 3;           // iterations
    unsigned m_cost_kib = 65536;       // 64 MiB
    unsigned parallel   = 4;
    unsigned hash_len   = 32;
    unsigned salt_len   = 16;
};

// Hash `password` with a freshly generated random salt. Writes a
// PHC-encoded hash string ($argon2id$...$...$...) into `out` on
// success. Returns 0 on success, negative libargon2 error otherwise.
int argon2_hash(const std::string& password,
                const Argon2Params& p,
                std::string* out);

// Verify `password` against a PHC-encoded hash. Returns 0 on match,
// negative on mismatch or malformed input.
int argon2_verify(const std::string& password, const std::string& encoded);

// 32 bytes from /dev/urandom, rendered as 64 lowercase hex chars.
// Returns an empty string if the OS entropy source is unavailable.
std::string new_session_token();

} // namespace budyk
