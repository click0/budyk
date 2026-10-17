// SPDX-License-Identifier: BSD-3-Clause
#include "storage/ring_check.h"

#include "storage/codec.h"

#include <cerrno>

namespace budyk {

namespace {

constexpr uint64_t kDayNs = 86400ULL * 1000000000ULL;
constexpr size_t   kMaxListedSlots = 8;

} // namespace

bool level_fits_ring(uint8_t tier, uint8_t level) {
    switch (tier) {
        case 1: return level == static_cast<uint8_t>(Level::L3);
        case 2: return level == static_cast<uint8_t>(Level::L2);
        case 3: return level == static_cast<uint8_t>(Level::L1);
        case kCustomRingTier: return level >= kFirstCustomLevel && level <= kMaxLevelId;
        default: return false;
    }
}

uint64_t count_unindexed(const RingFile& ring) {
    const uint64_t cap  = ring.capacity();
    const uint64_t widx = ring.write_index();
    if (cap == 0) return 0;
    std::vector<uint8_t> buf(record_size_for_sample());
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
        if (!read(widx + n, &s) || s.timestamp_nanos <= prev_ts) break;
        prev_ts = s.timestamp_nanos;
        ++n;
    }
    return n;
}

bool RingCheck::errors() const {
    if (header_rc != 0 || unknown_tier || record_size_differs || open_rc != 0) return true;
    return damaged > 0 || wrong_level > 0 || read_errors > 0 || unindexed > recovered;
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
        out->record_size_differs = true;
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
