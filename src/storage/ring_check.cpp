// SPDX-License-Identifier: BSD-3-Clause
#include "storage/ring_check.h"

#include "core/clock.h"
#include "storage/codec.h"

#include <cerrno>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace budyk {

namespace {

constexpr uint64_t kDayNs = 86400ULL * 1000000000ULL;
constexpr size_t   kMaxListedSlots = 8;

// fsync(2) of the directory at `path`: 0 or -errno. O_DIRECTORY and
// O_NONBLOCK: what the service user put under the name is no FIFO that
// holds root's start.
int sync_dir(const char* path) {
    const int fd = ::open(path, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -errno;
    const int rc = g_convert_dir_fsync(fd) != 0 ? -errno : 0;
    ::close(fd);
    return rc;
}

} // namespace

int (*g_convert_dir_fsync)(int fd) = ::fsync;

bool level_fits_ring(uint8_t tier, uint8_t level) {
    switch (tier) {
        case 1: return level == static_cast<uint8_t>(Level::L3);
        case 2: return level == static_cast<uint8_t>(Level::L2);
        case 3: return level == static_cast<uint8_t>(Level::L1);
        case kCustomRingTier: return level >= kFirstCustomLevel && level <= kMaxLevelId;
        default: return false;
    }
}

uint64_t count_unindexed(const RingFile& ring, uint64_t now_ns) {
    const uint64_t cap  = ring.capacity();
    const uint64_t widx = ring.write_index();
    if (cap == 0) return 0;
    if (now_ns == 0) now_ns = now_realtime_ns();
    // The ring's own record size: an older ring's records are smaller,
    // and read with this version's size nothing decoded, so a record
    // left unindexed by a crash was not seen before the conversion.
    std::vector<uint8_t> buf(ring.record_size() != 0 ? ring.record_size() : record_size_for_sample());
    auto read = [&](uint64_t logical, Sample* s) {
        return ring.read_at(logical % cap, buf.data(), buf.size()) == 0 &&
               record_decode(buf.data(), buf.size(), s) == 0 &&
               level_fits_ring(ring.tier(), static_cast<uint8_t>(s->level));
    };

    // The newest indexed record that reads, if any.
    const uint64_t indexed = widx < cap ? widx : cap;
    uint64_t prev_ts = 0;
    for (uint64_t k = 0; k < indexed; ++k) {
        Sample s{};
        if (read(widx - 1 - k, &s)) { prev_ts = s.timestamp_nanos; break; }
    }
    uint64_t n = 0;
    while (n < cap) {
        Sample s{};
        // Stamped after now: not written before this start. After the
        // clock stepped back further than the ring spans, the oldest
        // records are "newer" than the newest, and were counted in.
        if (!read(widx + n, &s) || s.timestamp_nanos <= prev_ts || s.timestamp_nanos > now_ns) break;
        prev_ts = s.timestamp_nanos;
        ++n;
    }
    return n;
}

int convert_ring(const char* path, uint8_t tier, uint32_t record_size,
                 uint64_t capacity, uint64_t* kept, uint64_t* dropped,
                 int* dir_sync_err) {
    *kept = *dropped = 0;
    if (dir_sync_err != nullptr) *dir_sync_err = 0;
    RingFile::Header h;
    int err = 0;
    if (RingFile::read_header(path, &h, &err) != 0) return err != 0 ? -err : -EINVAL;
    RingFile old;
    if (old.open_read_only(path, h.tier, h.record_size, h.capacity) != 0) {
        return old.last_errno() != 0 ? -old.last_errno() : -EINVAL;
    }
    const std::string tmp = std::string(path) + ".convert";
    ::unlink(tmp.c_str());                    // left by a conversion cut short
    RingFile out;
    if (out.open(tmp.c_str(), tier, record_size, capacity) != 0) {
        err = out.last_errno() != 0 ? out.last_errno() : EIO;
        old.close();                          // no destructor does it
        return -err;
    }

    const uint64_t widx   = old.write_index();
    // A record written but not indexed before a crash (a SIGKILL between
    // the record's pwrite and the index update) is counted in at start
    // (count_unindexed); the conversion runs before that, and copied the
    // indexed records only, so the record was lost when the next start
    // was a newer budyk. It is copied too, at the end; in a full ring it
    // took the oldest record's slot, which is gone.
    const uint64_t unidx  = count_unindexed(old);
    uint64_t       count  = widx < h.capacity ? widx : h.capacity;
    uint64_t       first  = widx - count;
    if (count + unidx > h.capacity) {
        first += count + unidx - h.capacity;
        count  = h.capacity - unidx;
    }
    const uint64_t total  = count + unidx;
    const uint64_t skip   = total > capacity ? total - capacity : 0;
    std::vector<uint8_t> in(h.record_size);
    uint8_t enc[1024];
    int rc = 0;
    for (uint64_t logical = first + skip; logical < widx + unidx; ++logical) {
        Sample s{};
        size_t len = 0;
        if (old.read_at(logical % h.capacity, in.data(), in.size()) != 0 ||
            record_decode(in.data(), in.size(), &s) != 0) {
            ++*dropped;
            continue;
        }
        if (record_encode(s, enc, sizeof(enc), &len) != 0) {
            rc = -EIO;
            break;
        }
        if (out.append(enc, len) != 0) {      // why, as the write said (ENOSPC): it was EIO
            rc = out.last_errno() != 0 ? -out.last_errno() : -EIO;
            break;
        }
        ++*kept;
    }
    *dropped += skip;
    // The converted ring on disk before it takes the old one's name, and
    // the rename in the directory after: a power cut soon after the
    // rename otherwise could find, on a filesystem that does not flush a
    // file renamed over another (XFS), the name with no data in it and
    // the old ring gone. Synced through its own descriptor: opened again
    // by name, a FIFO the service user put there held root's start.
    if (rc == 0) rc = out.sync();
    out.close();
    old.close();
    if (rc == 0 && ::rename(tmp.c_str(), path) != 0) rc = -errno;
    if (rc == 0) {
        // Renamed: the ring is converted either way, and a failed sync of
        // the directory here failed the start, which then found a ring of
        // this version and never synced it.
        const std::string p = path;
        const size_t slash = p.rfind('/');
        const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : p.substr(0, slash);
        const int d = sync_dir(dir.c_str());
        if (dir_sync_err != nullptr) *dir_sync_err = -d;
        return 0;
    }
    ::unlink(tmp.c_str());
    return rc;
}

bool RingCheck::errors() const {
    return errors_but_unindexed() || unindexed > recovered;
}

bool RingCheck::errors_but_unindexed() const {
    if (header_rc != 0 || unknown_tier || newer_format || open_rc != 0) return true;
    return damaged > 0 || wrong_level > 0 || read_errors > 0;
}

int check_ring(const char* path, bool repair, uint64_t now_ns, RingCheck* out) {
    *out = RingCheck{};
    out->header_rc = RingFile::read_header(path, &out->header, &out->header_errno);
    if (out->header_rc != 0) return 0;
    const RingFile::Header& h = out->header;
    if (h.tier != 1 && h.tier != 2 && h.tier != 3 && h.tier != kCustomRingTier) {
        out->unknown_tier = true;
        return 0;
    }
    if (h.record_size != record_size_for_sample()) {
        (h.record_size < record_size_for_sample() ? out->older_format : out->newer_format) = true;
        return 0;
    }

    RingFile ring;
    out->open_rc = repair ? ring.open(path, h.tier, h.record_size, h.capacity)
                          : ring.open_read_only(path, h.tier, h.record_size, h.capacity);
    if (out->open_rc != 0) {
        const int err = ring.last_errno();
        return repair && err != 0 ? -err : 0;
    }

    const uint64_t cap    = ring.capacity();
    const uint64_t widx   = ring.write_index();
    const uint64_t count  = widx < cap ? widx : cap;
    const uint64_t oldest = widx - count;
    std::vector<uint8_t> buf(h.record_size);
    uint64_t max_ts = 0;
    for (uint64_t logical = oldest; logical < widx; ++logical) {
        ++out->records;
        const uint64_t slot = logical % cap;
        if (ring.read_at(slot, buf.data(), buf.size()) != 0) {
            ++out->read_errors;
            continue;
        }
        Sample s{};
        if (record_decode(buf.data(), buf.size(), &s) != 0) {
            ++out->damaged;
            if (out->damaged_slots.size() < kMaxListedSlots) out->damaged_slots.push_back(slot);
            continue;
        }
        if (!level_fits_ring(h.tier, static_cast<uint8_t>(s.level))) ++out->wrong_level;
        if (s.timestamp_nanos < max_ts) ++out->out_of_order;
        if (now_ns != 0 && s.timestamp_nanos > now_ns + kDayNs) ++out->future;
        if (s.folded) ++out->folded;
        if (out->valid == 0 || s.timestamp_nanos < out->first_ts) out->first_ts = s.timestamp_nanos;
        if (s.timestamp_nanos > out->last_ts) out->last_ts = s.timestamp_nanos;
        if (s.timestamp_nanos > max_ts) max_ts = s.timestamp_nanos;
        ++out->valid;
    }

    out->unindexed = count_unindexed(ring);
    if (repair && out->unindexed > 0 && ring.advance_write_index(out->unindexed) == 0) {
        out->recovered = out->unindexed;
    }
    ring.close();
    return 0;
}

} // namespace budyk
