// SPDX-License-Identifier: BSD-3-Clause
#include "web/auth.h"

extern "C" {
#include <argon2.h>
}

#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace budyk {

namespace {

// Fill `buf` with `len` bytes from /dev/urandom. Returns true on success.
bool read_random(void* buf, size_t len) {
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    auto*  p         = static_cast<unsigned char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::read(fd, p, remaining);
        if (n <= 0) { ::close(fd); return false; }
        p         += n;
        remaining -= static_cast<size_t>(n);
    }
    ::close(fd);
    return true;
}

} // namespace

int argon2_hash(const std::string& password, const Argon2Params& p, std::string* out) {
    if (out == nullptr)                        return ARGON2_OUTPUT_PTR_NULL;
    if (p.salt_len == 0 || p.hash_len == 0)    return ARGON2_OUTPUT_TOO_SHORT;

    std::vector<unsigned char> salt(p.salt_len);
    if (!read_random(salt.data(), salt.size())) return ARGON2_SALT_TOO_SHORT;

    const size_t encoded_len = argon2_encodedlen(
        p.t_cost, p.m_cost_kib, p.parallel, p.salt_len, p.hash_len, Argon2_id);

    std::vector<char> encoded(encoded_len);
    int rc = argon2id_hash_encoded(
        p.t_cost, p.m_cost_kib, p.parallel,
        password.data(), password.size(),
        salt.data(),     salt.size(),
        p.hash_len,
        encoded.data(),  encoded.size());
    if (rc != ARGON2_OK) return rc;

    out->assign(encoded.data());   // up to first NUL
    return 0;
}

int argon2_verify(const std::string& password, const std::string& encoded) {
    if (encoded.empty()) return ARGON2_DECODING_FAIL;
    return argon2id_verify(encoded.c_str(), password.data(), password.size());
}

std::string new_session_token() {
    unsigned char buf[32];
    if (!read_random(buf, sizeof(buf))) return std::string{};

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(sizeof(buf) * 2);
    for (size_t i = 0; i < sizeof(buf); ++i) {
        out[i * 2]     = kHex[(buf[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[buf[i] & 0xF];
    }
    return out;
}

} // namespace budyk
