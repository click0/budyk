// SPDX-License-Identifier: BSD-3-Clause
#include "core/codec.h"
#include "core/sample.h"
#include "storage/codec.h"
#include "storage/ring_file.h"

#include <cassert>
#include <string>
#include <vector>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
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
        uint8_t buf[1024] = {0};
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
        uint8_t buf[1024] = {0};
        size_t  len = 0;
        assert(record_encode(in, buf, sizeof(buf), &len) == 0);
        buf[kRecordHeaderSize + 20] ^= 0x01;
        Sample out{};
        assert(record_decode(buf, len, &out) != 0);
    }

    // 4. CRC mismatch — flipped header byte (ts) is rejected.
    {
        Sample in = make_sample(1000, Level::L2, 0.5);
        uint8_t buf[1024] = {0};
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
                Sample s = make_sample((i + 1) * 1000, Level::L3, 0.1 * static_cast<double>(i + 1));
                uint8_t rec[1024] = {0};
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
                uint8_t rec[1024] = {0};
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
            uint8_t rec[1024] = {0};
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
            uint8_t rec[1024] = {0};
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
            // A different capacity means a different file size, which is
            // checked before the header, so this is -6, not -12.
            assert(rf.open(path, 1, (uint32_t)rsize, 8 /* wrong cap */) == -6);
            assert(rf.last_errno() == 0);                       // not a syscall failure
            assert(std::string(RingFile::describe(-12)).find("capacity") != std::string::npos);
            assert(std::string(RingFile::describe(-6)).find("size") != std::string::npos);
        }
        // A path that cannot be opened: the code says "open failed" and
        // last_errno() carries why.
        {
            RingFile rf;
            assert(rf.open("/nonexistent/dir/x.ring", 1, (uint32_t)rsize, 4) == -3);
            assert(rf.last_errno() == ENOENT);
            assert(std::string(RingFile::describe(-3)) == "open failed");
        }
        ::unlink(path);
    }

    // 8b. A new ring file is created mode 0600 (the daemon's history,
    //     like sessions.tsv), whatever the umask lets through.
    {
        char tmpl[] = "/tmp/budyk_ringmode_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        assert(dir != nullptr);
        const std::string path = std::string(dir) + "/r.ring";
        const mode_t old = ::umask(0);
        RingFile rf;
        assert(rf.open(path.c_str(), 1, 64, 4) == 0);
        ::umask(old);
        struct stat st{};
        assert(::stat(path.c_str(), &st) == 0);
        assert((st.st_mode & 0777) == 0600);
        rf.close();
        ::unlink(path.c_str());
        ::rmdir(dir);
    }

    // 9. read_at out of range + len mismatch rejected.
    {
        char path[256];
        tmp_path(path, sizeof(path));
        ::unlink(path);

        RingFile rf;
        assert(rf.open(path, 3, (uint32_t)rsize, 2) == 0);

        uint8_t rec[1024] = {0};
        assert(rf.read_at(5, rec, rsize) != 0);  // index >= capacity
        assert(rf.read_at(0, rec, rsize - 1) != 0);  // wrong length
        rf.close();
        ::unlink(path);
    }

    // 10. open_read_only: never creates a file; reads what open() wrote;
    //     append() and advance_write_index() refuse; the checks of open()
    //     apply. read_header(): the header without opening for writing,
    //     and its codes for a short file, bad magic and a size mismatch.
    //     advance_write_index() counts records in.
    {
        char path[256];
        tmp_path(path, sizeof(path));
        ::unlink(path);
        RingFile ro;
        assert(ro.open_read_only(path, 1, (uint32_t)rsize, 4) == -3);   // missing
        assert(ro.last_errno() == ENOENT && ::access(path, F_OK) != 0);

        RingFile rw;
        assert(rw.open(path, 1, (uint32_t)rsize, 4) == 0);
        assert(rw.tier() == 1);
        uint8_t rec[1024] = {0};
        size_t len = 0;
        assert(record_encode(make_sample(10, Level::L3, 1.0), rec, sizeof(rec), &len) == 0);
        assert(rw.append(rec, len) == 0);

        assert(ro.open_read_only(path, 2, (uint32_t)rsize, 4) == -10);   // wrong tier
        assert(ro.open_read_only(path, 1, (uint32_t)rsize, 4) == 0);
        assert(ro.write_index() == 1 && ro.tier() == 1);
        uint8_t back[1024] = {0};
        assert(ro.read_at(0, back, rsize) == 0 && std::memcmp(back, rec, rsize) == 0);
        assert(ro.append(rec, len) == -1);
        assert(ro.advance_write_index(1) == -1);

        RingFile::Header h;
        int err = -1;
        assert(RingFile::read_header(path, &h, &err) == 0 && err == 0);
        assert(h.tier == 1 && h.record_size == rsize && h.capacity == 4 && h.write_idx == 1);

        // A record written into the next slot, then counted in.
        assert(rw.advance_write_index(2) == 0);
        assert(rw.write_index() == 3 && ro.write_index() == 3);   // one header, mapped twice
        rw.close();
        ro.close();

        FILE* f = std::fopen(path, "r+b");
        assert(f != nullptr);
        std::fputc('X', f);                                       // magic
        std::fclose(f);
        assert(RingFile::read_header(path, &h, &err) == -8);
        assert(::truncate(path, 63) == 0);
        assert(RingFile::read_header(path, &h, &err) == -13);
        assert(std::string(RingFile::describe(-13)) == "file is shorter than a ring header");
        ::unlink(path);

        assert(rw.open(path, 1, (uint32_t)rsize, 4) == 0);
        rw.close();
        assert(::truncate(path, 64 + (off_t)rsize * 3) == 0);    // one slot short
        assert(RingFile::read_header(path, &h, &err) == -6);
        assert(::truncate(path, 64 + (off_t)rsize * 4 + 7) == 0); // 7 bytes over
        assert(RingFile::read_header(path, &h, &err) == -6);
        assert(RingFile::read_header("/nonexistent/x.ring", &h, &err) == -3 && err == ENOENT);
        ::unlink(path);
    }

    // 11. A ring file of the right size whose header is all zero bytes: a
    //     start that died between the ftruncate and the header (a full
    //     disk: the header was stored through the mapping into a hole of
    //     the sparse file, and that is SIGBUS). It used to stop every later
    //     start with "bad magic"; it holds nothing, and is made anew.
    //     Read-only, it is still refused.
    {
        char path[] = "/tmp/budyk_blank_XXXXXX";
        const int fd = ::mkstemp(path);
        assert(fd >= 0);
        assert(::ftruncate(fd, 64 + (off_t)rsize * 4) == 0);
        ::close(fd);
        RingFile ro;
        assert(ro.open_read_only(path, 1, (uint32_t)rsize, 4) == -8);
        RingFile rw;
        assert(rw.open(path, 1, (uint32_t)rsize, 4) == 0);
        assert(rw.write_index() == 0);
        std::vector<uint8_t> rec(rsize, 0x5a);
        assert(rw.append(rec.data(), rec.size()) == 0);
        rw.close();
        RingFile::Header h;
        int err = 0;
        assert(RingFile::read_header(path, &h, &err) == 0 && h.tier == 1 && h.write_idx == 1);
        ::unlink(path);
    }

    // 12. A write that fails says why (last_errno): a full disk stopped
    //     the history and nothing said what. RLIMIT_FSIZE below the first
    //     slot stands in for the full disk (EFBIG; SIGXFSZ ignored).
    {
        char path[] = "/tmp/budyk_full_XXXXXX";
        const int fd = ::mkstemp(path);
        assert(fd >= 0);
        ::close(fd);
        ::unlink(path);
        RingFile rw;
        assert(rw.open(path, 1, (uint32_t)rsize, 4) == 0);
        struct sigaction ign{}, old{};
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        assert(::sigaction(SIGXFSZ, &ign, &old) == 0);
        struct rlimit lim{}, old_lim{};
        assert(::getrlimit(RLIMIT_FSIZE, &old_lim) == 0);
        lim = old_lim;
        lim.rlim_cur = 64;
        assert(::setrlimit(RLIMIT_FSIZE, &lim) == 0);
        std::vector<uint8_t> rec(rsize, 0x5a);
        const int rc = rw.append(rec.data(), rec.size());
        assert(::setrlimit(RLIMIT_FSIZE, &old_lim) == 0);
        ::sigaction(SIGXFSZ, &old, nullptr);
        assert(rc == -3 && rw.last_errno() == EFBIG && rw.write_index() == 0);
        rw.close();
        ::unlink(path);
    }

    std::printf("test_ring_file: PASS (record=%zu bytes)\n", rsize);
    return 0;
}
