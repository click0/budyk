// SPDX-License-Identifier: BSD-3-Clause
#include "core/codec.h"
#include "core/sample.h"
#include "storage/codec.h"
#include "storage/ring_file.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace budyk;

static Sample make_sample(uint64_t ts_ns, Level lv, double load1) {
    Sample s{};
    s.timestamp_nanos = ts_ns;
    s.level           = lv;
    s.cpu.total_percent = 42.0;
    s.cpu.count         = 4;
    s.load.avg_1m       = load1;
    return s;
}

// Unique tempfile path; caller unlinks.
static void tmp_path(char* out, size_t cap) {
    std::snprintf(out, cap, "/tmp/budyk_ring_%d_%p", (int)getpid(), (void*)&out);
}

int main() {
    const size_t rsize = record_size_for_sample();
    assert(rsize == kRecordHeaderSize + sample_max_encoded_size());

    // 1. CRC32C — known Castagnoli vectors.
    {
        // Empty input: CRC-32C("") = 0x00000000.
        assert(crc32c("", 0) == 0x00000000U);
        // "123456789" → 0xE3069283 (standard CRC-32C check value).
        assert(crc32c("123456789", 9) == 0xE3069283U);
    }

    // 2. record_encode / record_decode roundtrip.
    {
        Sample in = make_sample(0x1122334455667788ULL, Level::L3, 1.25);
        uint8_t buf[512] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);
        assert(len == rsize);

        Sample out{};
        assert(record_decode(buf, len, &out) == 0);
        assert(out.timestamp_nanos == in.timestamp_nanos);
        assert(out.level           == in.level);
        assert(out.cpu.count       == in.cpu.count);
    }

    // 3. CRC mismatch — flipped payload byte is rejected.
    {
        Sample in = make_sample(1000, Level::L2, 0.5);
        uint8_t buf[512] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);
        buf[kRecordHeaderSize + 20] ^= 0x01;
        Sample out{};
        assert(record_decode(buf, len, &out) != 0);
    }

    // 4. CRC mismatch — flipped header byte (ts) is rejected.
    {
        Sample in = make_sample(1000, Level::L2, 0.5);
        uint8_t buf[512] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);
        buf[0] ^= 0xFF;
        Sample out{};
        assert(record_decode(buf, len, &out) != 0);
    }

    // 5. Too-small buffer on encode / decode is rejected.
    {
        Sample in = make_sample(1, Level::L1, 0.0);
        uint8_t tiny[8] = {0};
        size_t  len = 0;
        assert(record_encode(in, tiny, sizeof(tiny), &len) != 0);
        Sample out{};
        assert(record_decode(tiny, sizeof(tiny), &out)     != 0);
    }

    // 6. RingFile — fresh create, append, read, reopen preserves write_idx.
    {
        char path[256];
        tmp_path(path, sizeof(path));
        ::unlink(path);

        {
            RingFile rf;
            assert(rf.open(path, /*tier*/ 1, (uint32_t)rsize, /*cap*/ 8) == 0);
            assert(rf.write_index() == 0);
            assert(rf.count()       == 0);

            for (uint64_t i = 0; i < 5; ++i) {
                Sample s = make_sample((i + 1) * 1000, Level::L3, 0.1 * (i + 1));
                uint8_t rec[512] = {0};
                size_t  len = 0;
                assert(record_encode(s, rec, sizeof(rec), &len) == 0);
                assert(rf.append(rec, rsize) == 0);
            }
            assert(rf.write_index() == 5);
            assert(rf.count()       == 5);
            rf.close();
        }

        // Reopen: state carried through mmap-backed header.
        {
            RingFile rf;
            assert(rf.open(path, 1, (uint32_t)rsize, 8) == 0);
            assert(rf.write_index() == 5);

            for (uint64_t i = 0; i < 5; ++i) {
                uint8_t rec[512] = {0};
                assert(rf.read_at(i, rec, rsize) == 0);
                Sample out{};
                assert(record_decode(rec, rsize, &out) == 0);
                assert(out.timestamp_nanos == (i + 1) * 1000);
                assert(out.level           == Level::L3);
            }
            rf.close();
        }

        ::unlink(path);
    }

    // 7. RingFile wrap-around: capacity N, write 2N+k — slot 0 holds the
    //    (N+1)-th record from the second lap.
    {
        char path[256];
        tmp_path(path, sizeof(path));
        ::unlink(path);

        const uint64_t cap = 4;
        RingFile rf;
        assert(rf.open(path, 2, (uint32_t)rsize, cap) == 0);

        for (uint64_t i = 0; i < 9; ++i) {
            Sample s = make_sample((i + 1) * 100, Level::L2, 0.0);
            uint8_t rec[512] = {0};
            size_t  len = 0;
            assert(record_encode(s, rec, sizeof(rec), &len) == 0);
            assert(rf.append(rec, rsize) == 0);
        }
        assert(rf.write_index() == 9);
        assert(rf.count()       == cap);

        // slot i after 9 writes holds record with index (i + 9 - cap) ... actually
        // record at index k was placed in slot (k % cap). The surviving records
        // are k = 5,6,7,8 at slots 1,2,3,0 respectively.
        struct Expect { uint64_t slot; uint64_t ts; };
        Expect exp[] = { {1, 6 * 100}, {2, 7 * 100}, {3, 8 * 100}, {0, 9 * 100} };
        for (const auto& e : exp) {
            uint8_t rec[512] = {0};
            assert(rf.read_at(e.slot, rec, rsize) == 0);
            Sample out{};
            assert(record_decode(rec, rsize, &out) == 0);
            assert(out.timestamp_nanos == e.ts);
        }
        rf.close();
        ::unlink(path);
    }

    // 8. Validation on reopen: wrong tier / record_size / capacity rejected.
    {
        char path[256];
        tmp_path(path, sizeof(path));
        ::unlink(path);

        {
            RingFile rf;
            assert(rf.open(path, 1, (uint32_t)rsize, 4) == 0);
            rf.close();
        }
        {
            RingFile rf;
            assert(rf.open(path, 2 /* wrong tier */, (uint32_t)rsize, 4) != 0);
        }
        {
            RingFile rf;
            assert(rf.open(path, 1, (uint32_t)rsize + 1, 4) != 0);
        }
        {
            RingFile rf;
            assert(rf.open(path, 1, (uint32_t)rsize, 8 /* wrong cap */) != 0);
        }
        ::unlink(path);
    }

    // 9. read_at out of range + len mismatch rejected.
    {
        char path[256];
        tmp_path(path, sizeof(path));
        ::unlink(path);

        RingFile rf;
        assert(rf.open(path, 3, (uint32_t)rsize, 2) == 0);

        uint8_t rec[512] = {0};
        assert(rf.read_at(5, rec, rsize) != 0);  // index >= capacity
        assert(rf.read_at(0, rec, rsize - 1) != 0);  // wrong length
        rf.close();
        ::unlink(path);
    }

    std::printf("test_ring_file: PASS (record=%zu bytes)\n", rsize);
    return 0;
}
