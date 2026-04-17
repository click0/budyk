// SPDX-License-Identifier: BSD-3-Clause
#include "core/codec.h"
#include "core/sample.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace budyk;

static Sample make_sample() {
    Sample s{};
    s.timestamp_nanos        = 0x0123456789ABCDEFULL;
    s.level                  = Level::L3;
    s.cpu.total_percent      = 73.125;
    s.cpu.count              = 8;
    s.mem.total              = 16ULL * 1024 * 1024 * 1024;
    s.mem.available          = 4ULL  * 1024 * 1024 * 1024;
    s.mem.available_percent  = 25.0;
    s.swap.total             = 2ULL  * 1024 * 1024 * 1024;
    s.swap.used              = 512ULL * 1024 * 1024;
    s.swap.used_percent      = 25.0;
    s.load.avg_1m            = 0.75;
    s.load.avg_5m            = 1.25;
    s.load.avg_15m           = 2.5;
    s.uptime_seconds         = 123456.789;
    return s;
}

static bool bitwise_eq(double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

static void assert_samples_equal(const Sample& a, const Sample& b) {
    assert(a.timestamp_nanos == b.timestamp_nanos);
    assert(a.level == b.level);
    assert(bitwise_eq(a.cpu.total_percent, b.cpu.total_percent));
    assert(a.cpu.count == b.cpu.count);
    assert(a.mem.total == b.mem.total);
    assert(a.mem.available == b.mem.available);
    assert(bitwise_eq(a.mem.available_percent, b.mem.available_percent));
    assert(a.swap.total == b.swap.total);
    assert(a.swap.used == b.swap.used);
    assert(bitwise_eq(a.swap.used_percent, b.swap.used_percent));
    assert(bitwise_eq(a.load.avg_1m,  b.load.avg_1m));
    assert(bitwise_eq(a.load.avg_5m,  b.load.avg_5m));
    assert(bitwise_eq(a.load.avg_15m, b.load.avg_15m));
    assert(bitwise_eq(a.uptime_seconds, b.uptime_seconds));
}

int main() {
    const size_t enc_size = sample_max_encoded_size();

    // 1. Roundtrip — populated struct
    {
        Sample in = make_sample();
        uint8_t buf[512] = {0};
        size_t  len = 0;
        assert(sample_encode(&in, buf, sizeof(buf), &len) == 0);
        assert(len == enc_size);

        Sample out{};
        assert(sample_decode(buf, len, &out) == 0);
        assert_samples_equal(in, out);
    }

    // 2. Roundtrip — zero-initialised (except level, which must be 1..3)
    {
        Sample in{};
        in.level = Level::L1;
        uint8_t buf[512] = {0};
        size_t  len = 0;
        assert(sample_encode(&in, buf, sizeof(buf), &len) == 0);
        Sample out{};
        assert(sample_decode(buf, len, &out) == 0);
        assert_samples_equal(in, out);
    }

    // 3. Roundtrip — extreme values
    {
        Sample in{};
        in.level             = Level::L2;
        in.timestamp_nanos   = UINT64_MAX;
        in.cpu.count         = UINT32_MAX;
        in.cpu.total_percent = 99.9999;
        in.mem.total         = UINT64_MAX;
        in.load.avg_1m       = 1e300;
        in.load.avg_5m       = -1e-300;
        in.uptime_seconds    = 0.0;
        uint8_t buf[512] = {0};
        size_t  len = 0;
        assert(sample_encode(&in, buf, sizeof(buf), &len) == 0);
        Sample out{};
        assert(sample_decode(buf, len, &out) == 0);
        assert_samples_equal(in, out);
    }

    // 4. Buffer too small
    {
        Sample in = make_sample();
        uint8_t tiny[8] = {0};
        size_t len = 0;
        assert(sample_encode(&in, tiny, sizeof(tiny), &len) != 0);
    }

    // 5. Corrupt magic — rejected
    {
        Sample in = make_sample();
        uint8_t buf[512] = {0};
        size_t len = 0;
        assert(sample_encode(&in, buf, sizeof(buf), &len) == 0);
        buf[0] ^= 0xFF;
        Sample out{};
        assert(sample_decode(buf, len, &out) != 0);
    }

    // 6. Unsupported version — rejected
    {
        Sample in = make_sample();
        uint8_t buf[512] = {0};
        size_t len = 0;
        assert(sample_encode(&in, buf, sizeof(buf), &len) == 0);
        buf[8] = 0xEE; // version byte
        Sample out{};
        assert(sample_decode(buf, len, &out) != 0);
    }

    // 7. Out-of-range level — rejected. Level is at offset 8+4+4+8 = 24.
    {
        Sample in = make_sample();
        uint8_t buf[512] = {0};
        size_t len = 0;
        assert(sample_encode(&in, buf, sizeof(buf), &len) == 0);
        buf[24] = 9;
        Sample out{};
        assert(sample_decode(buf, len, &out) != 0);
        buf[24] = 0;
        assert(sample_decode(buf, len, &out) != 0);
    }

    // 8. Null arguments — all rejected cleanly
    {
        Sample in{};
        in.level = Level::L1;
        uint8_t buf[512] = {0};
        size_t  len = 0;
        assert(sample_encode(nullptr, buf, sizeof(buf), &len) != 0);
        assert(sample_encode(&in, nullptr, sizeof(buf), &len) != 0);
        assert(sample_encode(&in, buf, sizeof(buf), nullptr) != 0);
        Sample out{};
        assert(sample_decode(nullptr, 10, &out) != 0);
        assert(sample_decode(buf, 0, &out)      != 0);
        assert(sample_decode(buf, 10, nullptr)  != 0);
    }

    // 9. Short buffer on decode — rejected
    {
        Sample in = make_sample();
        uint8_t buf[512] = {0};
        size_t  len = 0;
        assert(sample_encode(&in, buf, sizeof(buf), &len) == 0);
        Sample out{};
        assert(sample_decode(buf, len - 1, &out) != 0);
    }

    // 10. Encoding is deterministic — same input ⇒ identical bytes
    {
        Sample in = make_sample();
        uint8_t a[512] = {0}, b[512] = {0};
        size_t la = 0, lb = 0;
        assert(sample_encode(&in, a, sizeof(a), &la) == 0);
        assert(sample_encode(&in, b, sizeof(b), &lb) == 0);
        assert(la == lb);
        assert(std::memcmp(a, b, la) == 0);
    }

    std::printf("test_codec: PASS (%zu bytes/record)\n", enc_size);
    return 0;
}
