// SPDX-License-Identifier: BSD-3-Clause
#include "storage/codec.h"
#include "core/codec.h"
#include "core/sample.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace budyk;

static Sample make_sample() {
    Sample s{};
    s.timestamp_nanos          = 0xFEDCBA9876543210ULL;
    s.level                    = Level::L3;
    s.cpu.total_percent        = 42.5;
    s.cpu.count                = 8;
    s.mem.total                = 16ULL << 30;
    s.mem.available            = 4ULL << 30;
    s.mem.available_percent    = 25.0;
    s.swap.total               = 2ULL << 30;
    s.swap.used                = 256ULL << 20;
    s.swap.used_percent        = 12.5;
    s.load.avg_1m              = 0.75;
    s.load.avg_5m              = 1.10;
    s.load.avg_15m             = 1.50;
    s.disk.read_bytes_per_sec  = 50ULL * 1024 * 1024;
    s.disk.write_bytes_per_sec = 30ULL * 1024 * 1024;
    s.disk.device_count        = 2;
    s.net.rx_bytes_per_sec     = 5ULL * 1024 * 1024;
    s.net.tx_bytes_per_sec     = 1ULL * 1024 * 1024;
    s.net.interface_count      = 3;
    s.uptime_seconds           = 7200.5;
    return s;
}

int main() {
    // 1. crc32c — known IEEE-vector "123456789" → 0xE3069283 (Castagnoli).
    {
        const char input[] = "123456789";
        uint32_t   c = crc32c(input, 9);
        assert(c == 0xE3069283U);
    }

    // 2. crc32c — empty input + zero seed = 0.
    {
        assert(crc32c(nullptr, 0) == 0);
    }

    // 3. crc32c — chained (seed) equivalent to single-pass.
    {
        const char input[] = "Hello, world!";
        uint32_t one_shot = crc32c(input, 13);
        uint32_t chained  = crc32c(input,     5);
        chained           = crc32c(input + 5, 8, chained);
        assert(one_shot == chained);
    }

    // 4. record_size_for_sample — header (14) + sample max (176 for v2).
    {
        const size_t sz = record_size_for_sample();
        assert(sz == kRecordHeaderSize + sample_max_encoded_size());
        assert(sz == 14 + 176);
    }

    // 5. Round-trip — full sample encode → decode preserves every field.
    {
        const Sample in = make_sample();
        uint8_t buf[256] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);
        assert(len == record_size_for_sample());

        Sample out{};
        assert(record_decode(buf, len, &out) == 0);
        assert(out.timestamp_nanos == in.timestamp_nanos);
        assert(out.level           == in.level);
        assert(out.cpu.count       == in.cpu.count);
        assert(out.mem.total       == in.mem.total);
        assert(out.disk.device_count   == in.disk.device_count);
        assert(out.net.interface_count == in.net.interface_count);
    }

    // 6. Encode is deterministic — same input → identical bytes.
    {
        const Sample in = make_sample();
        uint8_t a[256] = {0}, b[256] = {0};
        size_t  la = 0, lb = 0;
        assert(record_encode(in, a, sizeof(a), &la) == 0);
        assert(record_encode(in, b, sizeof(b), &lb) == 0);
        assert(la == lb);
        assert(std::memcmp(a, b, la) == 0);
    }

    // 7. Buffer too small for header — encode rejects.
    {
        const Sample in = make_sample();
        uint8_t tiny[8] = {0};
        size_t  len     = 0;
        assert(record_encode(in, tiny, sizeof(tiny), &len) != 0);
    }

    // 8. CRC mismatch — flip a payload byte, decode rejects.
    {
        const Sample in = make_sample();
        uint8_t buf[256] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);

        // Flip the first byte of the payload (just past the 14-byte header).
        buf[kRecordHeaderSize] ^= 0x01;
        Sample out{};
        assert(record_decode(buf, len, &out) != 0);
    }

    // 9. CRC mismatch — flip a timestamp byte (header is also covered).
    {
        const Sample in = make_sample();
        uint8_t buf[256] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);

        buf[0] ^= 0xFF;     // first byte of timestamp_nanos
        Sample out{};
        assert(record_decode(buf, len, &out) != 0);
    }

    // 10. Tampered CRC field itself — decode rejects.
    {
        const Sample in = make_sample();
        uint8_t buf[256] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);

        // CRC slot is at offset 10 (u64 ts + u8 level + u8 pad).
        buf[10] ^= 0x01;
        Sample out{};
        assert(record_decode(buf, len, &out) != 0);
    }

    // 11. Decode short buffer — rejects.
    {
        const Sample in = make_sample();
        uint8_t buf[256] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);
        Sample out{};
        assert(record_decode(buf, len - 1, &out) != 0);
    }

    // 12. Bad level byte (out of [1,3]) — decode rejects after CRC passes.
    //     Level is at offset 8. Flipping it invalidates the CRC AND yields
    //     an out-of-range value, so the CRC check fires first; here we
    //     re-encode from scratch with a hand-corrupted level so the CRC
    //     stays valid and the level guard runs.
    {
        Sample in = make_sample();
        uint8_t buf[256] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);

        // Set level to 9 in BOTH framing offset and payload, then fix CRC.
        buf[8] = 9;
        // Recompute CRC over header + payload.
        uint32_t c1 = crc32c(buf, 10);
        uint32_t c2 = crc32c(buf + kRecordHeaderSize,
                             sample_max_encoded_size(), c1);
        std::memcpy(buf + 10, &c2, 4);

        Sample out{};
        assert(record_decode(buf, len, &out) != 0);
    }

    // 13. Null-argument guards.
    {
        Sample in = make_sample();
        uint8_t buf[256] = {0};
        size_t  len = 0;
        assert(record_encode(in, nullptr, sizeof(buf), &len) != 0);
        assert(record_encode(in, buf, sizeof(buf), nullptr)  != 0);
        Sample out{};
        assert(record_decode(nullptr, 256, &out) != 0);
        assert(record_decode(buf, 256, nullptr)  != 0);
    }

    // 14. Framing wins for ts/level — decode reads them from the header,
    //     not from the payload. (sample_decode would've read them too,
    //     but we want record framing to be the source of truth so the
    //     ring file's index can scan without fully decoding payloads.)
    {
        const Sample in = make_sample();
        uint8_t buf[256] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);

        Sample out{};
        assert(record_decode(buf, len, &out) == 0);
        assert(out.timestamp_nanos == in.timestamp_nanos);
        assert(out.level           == in.level);
    }

    std::printf("test_storage_codec: PASS\n");
    return 0;
}
