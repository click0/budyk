// SPDX-License-Identifier: BSD-3-Clause
#include "web/auth.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <unordered_set>

using namespace budyk;

int main() {
    // Use cheap params in tests — OWASP defaults take >100ms per hash.
    Argon2Params fast;
    fast.t_cost     = 1;
    fast.m_cost_kib = 1 << 10;    // 1 MiB
    fast.parallel   = 1;
    fast.hash_len   = 16;
    fast.salt_len   = 8;

    // 1. Hash a password; output starts with the Argon2id PHC prefix.
    std::string h1;
    assert(argon2_hash("correct horse battery staple", fast, &h1) == 0);
    assert(!h1.empty());
    assert(h1.rfind("$argon2id$", 0) == 0);

    // 2. Verify the correct password.
    assert(argon2_verify("correct horse battery staple", h1) == 0);

    // 3. Reject the wrong password.
    assert(argon2_verify("wrong password", h1) != 0);

    // 4. Salt is random — re-hashing the same password yields a different encoded string.
    std::string h2;
    assert(argon2_hash("correct horse battery staple", fast, &h2) == 0);
    assert(h1 != h2);
    // …but both verify.
    assert(argon2_verify("correct horse battery staple", h2) == 0);

    // 5. Malformed encoded input is rejected cleanly.
    assert(argon2_verify("anything", "")                           != 0);
    assert(argon2_verify("anything", "not-a-phc-string")           != 0);
    assert(argon2_verify("anything", "$argon2id$v=19$m=1,t=1,p=1") != 0);

    // 6. Null out-pointer on hash is rejected.
    assert(argon2_hash("x", fast, nullptr) != 0);

    // 7. Session tokens: 64 hex chars, unique across draws.
    std::unordered_set<std::string> seen;
    for (int i = 0; i < 16; ++i) {
        std::string t = new_session_token();
        assert(t.size() == 64);
        for (char c : t) {
            assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
        assert(seen.insert(t).second);  // never repeated
    }

    std::printf("test_auth: PASS\n");
    return 0;
}
