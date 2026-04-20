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
    s.disk.read_bytes_per_sec  = 123ULL * 1024 * 1024;
    s.disk.write_bytes_per_sec =  45ULL * 1024 * 1024;
    s.disk.device_count        = 3;
    s.net.rx_bytes_per_sec     = 987654321ULL;
    s.net.tx_bytes_per_sec     = 123456789ULL;
    s.net.interface_count      = 2;
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
    assert(a.disk.read_bytes_per_sec  == b.disk.read_bytes_per_sec);
    assert(a.disk.write_bytes_per_sec == b.disk.write_bytes_per_sec);
    assert(a.disk.device_count        == b.disk.device_count);
    assert(a.net.rx_bytes_per_sec     == b.net.rx_bytes_per_sec);
    assert(a.net.tx_bytes_per_sec     == b.net.tx_bytes_per_sec);
    assert(a.net.interface_count      == b.net.interface_count);
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

    // 11. v1 backward compat — a hand-rolled v1 record (128 bytes, version=1)
    //     decodes successfully, with disk/net fields zeroed out.
    {
        constexpr size_t kV1Size = 128;
        uint8_t buf[kV1Size] = {0};
        uint8_t* p = buf;
        const uint8_t magic[8] = {'B','D','Y','K','S','M','P','L'};
        std::memcpy(p, magic, 8); p += 8;
        uint32_t ver = 1; std::memcpy(p, &ver, 4); p += 4;     // version = 1
        p += 4;                                                 // flags
        uint64_t ts = 0x0102030405060708ULL; std::memcpy(p, &ts, 8); p += 8;
        *p++ = 2;                                               // level = L2
        p += 7;                                                 // pad
        double cpu_pct = 55.5; std::memcpy(p, &cpu_pct, 8); p += 8;
        uint32_t cpu_ct = 4; std::memcpy(p, &cpu_ct, 4); p += 4;
        p += 4;                                                 // pad
        uint64_t mt = 1ULL << 30; std::memcpy(p, &mt, 8); p += 8;
        uint64_t ma = 1ULL << 29; std::memcpy(p, &ma, 8); p += 8;
        double mp = 50.0; std::memcpy(p, &mp, 8); p += 8;
        // Swap + load + uptime — leave zeros, they're valid.
        (void)p;

        Sample out{};
        int rc = sample_decode(buf, kV1Size, &out);
        assert(rc == 0);
        assert(out.timestamp_nanos == 0x0102030405060708ULL);
        assert(out.level == Level::L2);
        assert(out.cpu.count == 4);
        assert(bitwise_eq(out.cpu.total_percent, 55.5));
        assert(out.mem.total     == (1ULL << 30));
        assert(out.mem.available == (1ULL << 29));
        // Disk + net must be zeroed — v1 carried no such data.
        assert(out.disk.read_bytes_per_sec  == 0);
        assert(out.disk.write_bytes_per_sec == 0);
        assert(out.disk.device_count        == 0);
        assert(out.net.rx_bytes_per_sec     == 0);
        assert(out.net.tx_bytes_per_sec     == 0);
        assert(out.net.interface_count      == 0);
    }

    // 12. v2 with length that truncates the disk/net tail — rejected.
    {
        Sample in = make_sample();
        uint8_t buf[512] = {0};
        size_t  len = 0;
        assert(sample_encode(&in, buf, sizeof(buf), &len) == 0);
        // Feed decode a length that's smaller than the v2 encoded size
        // but bigger than the v1 floor — the version byte says v2, so
        // the tail must be present.
        Sample out{};
        assert(sample_decode(buf, len - 1, &out) != 0);
    }

    std::printf("test_codec: PASS (%zu bytes/record)\n", enc_size);
    return 0;
}
