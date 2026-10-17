// SPDX-License-Identifier: BSD-3-Clause
// check_ring() and count_unindexed(): budyk fsck and the start-up
// recovery of records written past write_idx (a crash between the
// pwrite and the index update).

#include "core/codec.h"
#include "core/sample.h"
#include "storage/codec.h"
#include "storage/ring_check.h"
#include "storage/ring_file.h"
#include "storage/tier_manager.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

using namespace budyk;

namespace {

const size_t kRec = record_size_for_sample();

std::string tmp_dir() {
    char tmpl[] = "/tmp/budyk_rc_XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    assert(d != nullptr);
    return d;
}

void rmrf(const std::string& d) {
    const std::string cmd = "rm -rf " + d;
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "rm -rf %s failed\n", d.c_str());
}

// A ring of `cap` records with the given header tier, holding samples
// of `level` at timestamps ts0, ts0 + 1, ... (n of them).
void make_ring(const std::string& path, uint8_t tier, uint64_t cap, Level level,
               uint64_t ts0, uint64_t n) {
    RingFile r;
    assert(r.open(path.c_str(), tier, static_cast<uint32_t>(kRec), cap) == 0);
    uint8_t buf[512];
    for (uint64_t i = 0; i < n; ++i) {
        Sample s{};
        s.timestamp_nanos = ts0 + i;
        s.level = level;
        size_t len = 0;
        assert(record_encode(s, buf, sizeof(buf), &len) == 0);
        assert(r.append(buf, len) == 0);
    }
    r.close();
}

void poke(const std::string& path, off_t off, const void* data, size_t len) {
    const int fd = ::open(path.c_str(), O_RDWR);
    assert(fd >= 0);
    assert(::pwrite(fd, data, len, off) == static_cast<ssize_t>(len));
    ::close(fd);
}

// write_idx lives at offset 28 of the header (little-endian u64).
void set_write_idx(const std::string& path, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
    poke(path, 28, b, 8);
}

// Rewrites the record in `slot` as `s` (a valid record).
void put_record(const std::string& path, uint64_t slot, const Sample& s) {
    uint8_t buf[512];
    size_t len = 0;
    assert(record_encode(s, buf, sizeof(buf), &len) == 0);
    poke(path, static_cast<off_t>(64 + slot * kRec), buf, len);
}

void flip_byte(const std::string& path, uint64_t slot) {
    const uint8_t x = 0x5A;
    poke(path, static_cast<off_t>(64 + slot * kRec + 40), &x, 1);
}

// A record as budyk 0.6 wrote it: codec v7 (the first 256 bytes of a v8
// payload, whose fields only append), version 7, its own CRC.
constexpr size_t kV7Payload = 256;
std::vector<uint8_t> v7_record(const Sample& s) {
    uint8_t payload[1024];
    size_t  len = 0;
    assert(sample_encode(&s, payload, sizeof(payload), &len) == 0);
    payload[8] = 7; payload[9] = payload[10] = payload[11] = 0;   // version
    std::vector<uint8_t> rec(kRecordHeaderSize + kV7Payload, 0);
    for (int i = 0; i < 8; ++i) rec[static_cast<size_t>(i)] = static_cast<uint8_t>(s.timestamp_nanos >> (8 * i));
    rec[8] = static_cast<uint8_t>(s.level);
    rec[9] = s.folded ? kRecordFolded : 0;
    std::memcpy(rec.data() + kRecordHeaderSize, payload, kV7Payload);
    uint32_t c = crc32c(rec.data(), 10);
    c = crc32c(rec.data() + kRecordHeaderSize, kV7Payload, c);
    for (int i = 0; i < 4; ++i) rec[10 + static_cast<size_t>(i)] = static_cast<uint8_t>(c >> (8 * i));
    return rec;
}

// A ring of v7 records (270 bytes), `n` of them at ts0, ts0 + 1, ...
void make_v7_ring(const std::string& path, uint8_t tier, uint64_t cap, Level level,
                  uint64_t ts0, uint64_t n) {
    RingFile r;
    assert(r.open(path.c_str(), tier, static_cast<uint32_t>(kRecordHeaderSize + kV7Payload), cap) == 0);
    for (uint64_t i = 0; i < n; ++i) {
        Sample s{};
        s.timestamp_nanos = ts0 + i;
        s.level = level;
        s.cpu.total_percent = static_cast<double>(i);
        s.proc.total = 100 + static_cast<uint32_t>(i);
        s.folded = (i % 2) == 1;
        const auto rec = v7_record(s);
        assert(r.append(rec.data(), rec.size()) == 0);
    }
    r.close();
}

RingCheck check(const std::string& path, bool repair) {
    RingCheck c;
    assert(check_ring(path.c_str(), repair, 0, &c) == 0);
    return c;
}

} // namespace

int main() {
    // 1. A clean ring: every record valid, nothing to repair.
    {
        const std::string d = tmp_dir(), p = d + "/tier1.ring";
        make_ring(p, 1, 16, Level::L3, 1000, 10);
        const RingCheck c = check(p, false);
        assert(c.header_rc == 0 && !c.errors());
        assert(c.records == 10 && c.valid == 10 && c.damaged == 0 && c.unindexed == 0);
        assert(c.first_ts == 1000 && c.last_ts == 1009);
        rmrf(d);
    }

    // 2. A crash between pwrite and the index update: the record is in
    //    its slot, write_idx one short. A check reports it (an error,
    //    left); --repair counts it in, and the ring is clean after.
    {
        const std::string d = tmp_dir(), p = d + "/tier3.ring";
        make_ring(p, 3, 16, Level::L1, 1000, 10);
        set_write_idx(p, 9);
        RingCheck c = check(p, false);
        assert(c.records == 9 && c.unindexed == 1 && c.recovered == 0 && c.errors());
        c = check(p, true);
        assert(c.unindexed == 1 && c.recovered == 1 && !c.errors());
        c = check(p, false);
        assert(c.records == 10 && c.unindexed == 0 && !c.errors());

        // A lost header page: several records past write_idx, all counted.
        set_write_idx(p, 6);
        c = check(p, true);
        assert(c.unindexed == 4 && c.recovered == 4);

        // Past write_idx, a valid and newer record of another level is not
        // this ring's: it is not counted in.
        Sample other{};
        other.level = Level::L3;
        other.timestamp_nanos = 2000;
        put_record(p, 10, other);
        c = check(p, false);
        assert(c.unindexed == 0 && !c.errors());
        rmrf(d);
    }

    // 3. A wrapped ring: past write_idx lies the oldest record, which is
    //    no unindexed one. With one record written but not counted, that
    //    one is recovered and the run stops at the oldest.
    {
        const std::string d = tmp_dir(), p = d + "/tier1.ring";
        make_ring(p, 1, 8, Level::L3, 1000, 21);         // write_idx 21, slots hold 13..20
        RingCheck c = check(p, false);
        assert(c.records == 8 && c.valid == 8 && c.unindexed == 0 && !c.errors());
        assert(c.first_ts == 1013 && c.last_ts == 1020);
        set_write_idx(p, 20);
        c = check(p, true);
        assert(c.unindexed == 1 && c.recovered == 1);
        rmrf(d);
    }

    // 4. Damaged records (a flipped payload byte), a record of another
    //    level, a clock step back: reported, nothing repaired. Damage and
    //    the wrong level are errors; the clock step is a note.
    {
        const std::string d = tmp_dir(), p = d + "/tier2.ring";
        make_ring(p, 2, 16, Level::L2, 1000, 10);
        flip_byte(p, 3);
        flip_byte(p, 7);
        RingCheck c = check(p, true);
        assert(c.damaged == 2 && c.valid == 8 && c.errors());
        assert(c.damaged_slots.size() == 2 && c.damaged_slots[0] == 3 && c.damaged_slots[1] == 7);
        assert(c.recovered == 0);

        Sample s{};
        s.level = Level::L1;
        s.timestamp_nanos = 1003;
        put_record(p, 3, s);                       // an L1 record in the L2 ring
        s.level = Level::L2;
        s.timestamp_nanos = 500;
        put_record(p, 7, s);                       // the clock stepped back
        c = check(p, false);
        assert(c.damaged == 0 && c.wrong_level == 1 && c.out_of_order == 1 && c.errors());
        assert(c.first_ts == 500);

        s.level = Level::L2;
        s.timestamp_nanos = 1003;
        put_record(p, 3, s);
        c = check(p, false);
        assert(c.wrong_level == 0 && c.out_of_order == 1 && !c.errors());   // a note only
        rmrf(d);
    }

    // 5. Custom-level rings take any custom id (a config reorder renumbers
    //    the levels); a future timestamp is a note.
    {
        const std::string d = tmp_dir(), p = d + "/level-burst.ring";
        make_ring(p, kCustomRingTier, 16, static_cast<Level>(kFirstCustomLevel + 3), 1000, 5);
        RingCheck c;
        assert(check_ring(p.c_str(), false, 999, &c) == 0);
        assert(!c.errors() && c.valid == 5 && c.future == 0);
        Sample s{};
        s.level = static_cast<Level>(kFirstCustomLevel);
        s.timestamp_nanos = 1005 + 2 * 86400ULL * 1000000000ULL;
        put_record(p, 5, s);
        set_write_idx(p, 6);
        assert(check_ring(p.c_str(), false, 1000, &c) == 0);
        assert(c.future == 1 && !c.errors());
        assert(level_fits_ring(kCustomRingTier, kFirstCustomLevel) &&
               !level_fits_ring(kCustomRingTier, 3) && !level_fits_ring(1, 1) &&
               level_fits_ring(1, 3) && !level_fits_ring(9, 3));
        rmrf(d);
    }

    // 6. What cannot be checked: a header that is not a ring's, an unknown
    //    tier byte, records of another size (another budyk version).
    {
        const std::string d = tmp_dir(), p = d + "/tier1.ring";
        make_ring(p, 1, 4, Level::L3, 1000, 2);
        poke(p, 0, "XXXX", 4);
        RingCheck c = check(p, false);
        assert(c.header_rc == -8 && c.errors());

        make_ring(d + "/odd.ring", 0x42, 4, Level::L3, 1000, 1);
        c = check(d + "/odd.ring", false);
        assert(c.unknown_tier && c.errors());

        RingFile r;
        assert(r.open((d + "/old.ring").c_str(), 1, static_cast<uint32_t>(kRec - 14), 4) == 0);
        r.close();
        c = check(d + "/old.ring", false);
        assert(c.older_format && !c.errors());

        RingCheck missing;
        assert(check_ring((d + "/none.ring").c_str(), false, 0, &missing) == 0);
        assert(missing.header_rc == -3 && missing.header_errno == ENOENT);
        rmrf(d);
    }

    // 7. TierManager::init() counts unindexed records in and says which
    //    ring: a daemon crash loses no record that reached its slot.
    {
        const std::string d = tmp_dir();
        TierManager tm;
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);
        for (uint64_t ts = 1; ts <= 5; ++ts) {
            Sample s{};
            s.timestamp_nanos = ts;
            s.level = Level::L3;
            assert(tm.store(s) == 0);
        }
        tm.close();
        set_write_idx(d + "/tier1.ring", 4);
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);
        assert(tm.recovered().size() == 1);
        assert(tm.recovered()[0].ring == "tier1.ring" && tm.recovered()[0].records == 1);
        assert(tm.tier1_count() == 5);
        tm.close();
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);
        assert(tm.recovered().empty());
        tm.close();
        rmrf(d);
    }

    // 8. convert_ring(): a v7 ring (budyk 0.6, 270-byte records) becomes
    //    a ring of this version's records, the fields v7 had intact, the
    //    newer ones zero, the v8 groups set to what v7 carried, the folded
    //    flag kept. With a smaller capacity the newest records stay. A
    //    damaged record is dropped; a .convert left by an interrupted run
    //    is replaced; fsck calls such a ring an older format, no error.
    {
        const std::string d = tmp_dir(), p = d + "/tier1.ring";
        make_v7_ring(p, 1, 32, Level::L3, 1000, 20);
        const uint8_t flip = 0x5A;                // damage slot 3 (dropped anyway)
        poke(p, static_cast<off_t>(64 + 3 * (kRecordHeaderSize + kV7Payload) + 40), &flip, 1);
        RingCheck c = check(p, false);
        assert(c.older_format && !c.errors());
        FILE* stale = std::fopen((p + ".convert").c_str(), "w");   // a leftover
        assert(stale != nullptr);
        std::fputs("stale", stale);
        std::fclose(stale);
        uint64_t kept = 0, dropped = 0;
        assert(convert_ring(p.c_str(), 1, static_cast<uint32_t>(kRec), 10, &kept, &dropped) == 0);
        assert(kept == 10 && dropped == 10);     // 20 - 10 oldest; slot 3 was among them
        assert(::access((p + ".convert").c_str(), F_OK) != 0);
        c = check(p, false);
        assert(!c.older_format && !c.errors() && c.valid == 10);
        assert(c.first_ts == 1010 && c.last_ts == 1019);

        RingFile r;
        assert(r.open_read_only(p.c_str(), 1, static_cast<uint32_t>(kRec), 10) == 0);
        std::vector<uint8_t> buf(kRec);
        Sample s{};
        assert(r.read_at(0, buf.data(), buf.size()) == 0 && record_decode(buf.data(), buf.size(), &s) == 0);
        assert(s.timestamp_nanos == 1010 && s.cpu.total_percent == 10.0 && s.proc.total == 110);
        assert(!s.folded && s.groups == kGroupsV7 && !s.tcp.present && s.proc.zombie == 0);
        assert(r.read_at(1, buf.data(), buf.size()) == 0 && record_decode(buf.data(), buf.size(), &s) == 0);
        assert(s.folded);
        r.close();

        // All of them fit: none dropped; a damaged one is.
        make_v7_ring(d + "/tier3.ring", 3, 32, Level::L1, 5000, 6);
        const uint8_t x = 0x5A;
        poke(d + "/tier3.ring", static_cast<off_t>(64 + 2 * (kRecordHeaderSize + kV7Payload) + 40), &x, 1);
        assert(convert_ring((d + "/tier3.ring").c_str(), 3, static_cast<uint32_t>(kRec), 64,
                            &kept, &dropped) == 0);
        assert(kept == 5 && dropped == 1);
        rmrf(d);
    }

    // 9. TierManager::init() converts the rings an older budyk left and
    //    says so; the next init has nothing to convert. A ring with
    //    larger records (a newer budyk) is not touched: init fails and
    //    says why.
    {
        const std::string d = tmp_dir();
        make_v7_ring(d + "/tier1.ring", 1, 64, Level::L3, 1000, 30);
        make_v7_ring(d + "/tier2.ring", 2, 64, Level::L2, 1000, 3);
        TierManager tm;
        assert(tm.init(d.c_str(), 1, 1, 1) == 0);
        assert(tm.converted().size() == 2);
        assert(tm.converted()[0].ring == "tier1.ring" && tm.converted()[0].from_record_size == 270);
        assert(tm.converted()[0].kept == 30 && tm.converted()[0].dropped == 0);
        assert(tm.tier1_count() == 30 && tm.tier2_count() == 3);
        tm.close();
        assert(tm.init(d.c_str(), 1, 1, 1) == 0 && tm.converted().empty());
        tm.close();

        RingFile big;
        assert(big.open((d + "/tier3.ring.big").c_str(), 3, static_cast<uint32_t>(kRec + 8), 4) == 0);
        big.close();
        assert(::rename((d + "/tier3.ring.big").c_str(), (d + "/tier3.ring").c_str()) == 0);
        assert(tm.init(d.c_str(), 1, 1, 1) != 0);
        assert(tm.last_error().find("written by a newer budyk") != std::string::npos);
        rmrf(d);
    }

    std::printf("test_ring_check: PASS\n");
    return 0;
}
