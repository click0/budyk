// SPDX-License-Identifier: BSD-3-Clause
#include "storage/tier_manager.h"

#include "storage/codec.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace budyk {

namespace {

// Capacities arrive in MiB; convert to record counts using the codec's
// per-record size. Floor at 1 record so an absurdly tiny budget still
// produces a usable ring (mostly for tests).
// Header tier byte for custom-level rings (built-ins use 1..3).
constexpr uint8_t kCustomRingTier = 0x80;

uint64_t records_from_mb(int mb, size_t record_size) {
    if (mb <= 0 || record_size == 0) return 1;
    const uint64_t bytes = static_cast<uint64_t>(mb) * 1024ULL * 1024ULL;
    const uint64_t cap   = bytes / record_size;
    return cap == 0 ? 1 : cap;
}

bool join_path(char* out, size_t cap, const char* dir, const char* leaf) {
    int n = std::snprintf(out, cap, "%s/%s", dir, leaf);
    return n > 0 && static_cast<size_t>(n) < cap;
}

} // namespace

int TierManager::init(const char* data_dir,
                      int tier1_max_mb,
                      int tier2_max_mb,
                      int tier3_max_mb,
                      const std::vector<LevelRingSpec>& custom) {
    if (data_dir == nullptr) return -1;
    if (ready_)              return -2;   // already initialised

    const uint32_t record_size = static_cast<uint32_t>(record_size_for_sample());
    const uint64_t cap1 = records_from_mb(tier1_max_mb, record_size);
    const uint64_t cap2 = records_from_mb(tier2_max_mb, record_size);
    const uint64_t cap3 = records_from_mb(tier3_max_mb, record_size);

    char path[1024];

    if (!join_path(path, sizeof(path), data_dir, "tier1.ring")) return -3;
    if (tier1_.open(path, /*tier*/1, record_size, cap1) != 0)   return -4;

    if (!join_path(path, sizeof(path), data_dir, "tier2.ring")) {
        tier1_.close();
        return -5;
    }
    if (tier2_.open(path, /*tier*/2, record_size, cap2) != 0) {
        tier1_.close();
        return -6;
    }

    if (!join_path(path, sizeof(path), data_dir, "tier3.ring")) {
        tier2_.close();
        tier1_.close();
        return -7;
    }
    if (tier3_.open(path, /*tier*/3, record_size, cap3) != 0) {
        tier2_.close();
        tier1_.close();
        return -8;
    }

    // Custom levels. Every custom ring has the same header tier byte; the
    // file name identifies the level.
    for (const auto& spec : custom) {
        const std::string leaf = "level-" + spec.name + ".ring";
        auto ring = std::unique_ptr<RingFile>(new RingFile());
        if (!join_path(path, sizeof(path), data_dir, leaf.c_str()) ||
            ring->open(path, kCustomRingTier, record_size,
                       records_from_mb(spec.max_mb, record_size)) != 0) {
            for (auto& c : custom_) c.ring->close();
            custom_.clear();
            tier3_.close();
            tier2_.close();
            tier1_.close();
            return -9;
        }
        custom_.push_back(CustomRing{spec.level_id, std::move(ring)});
    }

    ready_ = true;
    return 0;
}

int TierManager::store(const Sample& s) {
    if (!ready_) return -1;

    // Pick the destination first — fail fast for unknown levels rather
    // than wasting an encode pass.
    RingFile* target = const_cast<RingFile*>(ring_for(s.level));
    if (target == nullptr) return -2;

    // Headroom over the current record size (14 + 256 = 270 for codec
    // v7) so a future codec bump doesn't silently overflow here.
    uint8_t buf[512];
    size_t  len = 0;
    const int rc = record_encode(s, buf, sizeof(buf), &len);
    if (rc != 0) return -3;

    if (target->append(buf, len) != 0) return -4;
    return 0;
}

void TierManager::close() {
    if (!ready_) return;
    for (auto& c : custom_) c.ring->close();
    custom_.clear();
    tier3_.close();
    tier2_.close();
    tier1_.close();
    ready_ = false;
}

const RingFile* TierManager::ring_for(Level level) const {
    switch (level) {
        case Level::L3: return &tier1_;
        case Level::L2: return &tier2_;
        case Level::L1: return &tier3_;
    }
    for (const auto& c : custom_) {
        if (c.level_id == static_cast<uint8_t>(level)) return c.ring.get();
    }
    return nullptr;
}

int TierManager::query(int tier, uint64_t since_ns, uint64_t until_ns,
                       std::size_t max_records, std::vector<Sample>* out) const {
    if (!ready_ || out == nullptr || max_records == 0) return -1;

    const RingFile* ring = nullptr;
    switch (tier) {
        case 1: ring = &tier1_; break;
        case 2: ring = &tier2_; break;
        case 3: ring = &tier3_; break;
        default: return -1;
    }
    return query_ring(ring, /*override_level*/-1, since_ns, until_ns, max_records, out);
}

int TierManager::query_level(Level level, uint64_t since_ns, uint64_t until_ns,
                             std::size_t max_records, std::vector<Sample>* out) const {
    if (!ready_ || out == nullptr || max_records == 0) return -1;
    const RingFile* ring = ring_for(level);
    if (ring == nullptr) return -1;
    const int override_level = static_cast<uint8_t>(level) >= kFirstCustomLevel
                             ? static_cast<int>(level) : -1;
    return query_ring(ring, override_level, since_ns, until_ns, max_records, out);
}

int TierManager::query_all(uint64_t since_ns, uint64_t until_ns,
                           std::size_t max_records, std::vector<Sample>* out) const {
    if (!ready_ || out == nullptr || max_records == 0) return -1;

    std::vector<Sample> merged;
    query_ring_spread(&tier1_, -1, since_ns, until_ns, max_records, &merged);
    query_ring_spread(&tier2_, -1, since_ns, until_ns, max_records, &merged);
    query_ring_spread(&tier3_, -1, since_ns, until_ns, max_records, &merged);
    for (const auto& c : custom_) {
        query_ring_spread(c.ring.get(), c.level_id, since_ns, until_ns,
                          max_records, &merged);
    }
    std::sort(merged.begin(), merged.end(), [](const Sample& a, const Sample& b) {
        return a.timestamp_nanos < b.timestamp_nanos;
    });

    if (merged.size() > max_records) {
        // Thin by time: newest sample per slice of the window.
        const uint64_t lo = merged.front().timestamp_nanos;
        const uint64_t hi = merged.back().timestamp_nanos;
        const uint64_t span  = hi - lo + 1;
        std::vector<Sample> thinned;
        thinned.reserve(max_records);
        uint64_t last_slice = UINT64_MAX;
        for (const auto& s : merged) {
            const uint64_t slice = static_cast<uint64_t>(
                (static_cast<long double>(s.timestamp_nanos - lo) * max_records) / span);
            if (slice == last_slice) thinned.back() = s;
            else { thinned.push_back(s); last_slice = slice; }
        }
        merged.swap(thinned);
    }

    out->reserve(out->size() + merged.size());
    out->insert(out->end(), merged.begin(), merged.end());
    return static_cast<int>(merged.size());
}

int TierManager::query_ring_spread(const RingFile* ring, int override_level,
                                   uint64_t since_ns, uint64_t until_ns,
                                   std::size_t max_records,
                                   std::vector<Sample>* out) const {
    const uint64_t cap  = ring->capacity();
    const uint64_t widx = ring->write_index();
    if (cap == 0 || widx == 0) return 0;
    const uint64_t valid  = widx < cap ? widx : cap;
    const uint64_t oldest = widx - valid;          // logical indices
    const uint64_t newest = widx - 1;

    const std::size_t rsize = record_size_for_sample();
    std::vector<uint8_t> buf(rsize);
    auto read = [&](uint64_t logical, Sample* s) {
        return ring->read_at(logical % cap, buf.data(), rsize) == 0 &&
               record_decode(buf.data(), rsize, s) == 0;
    };
    // Timestamp at `logical`, stepping forward past a record that doesn't
    // decode (e.g. the one being written). False if none up to `limit`.
    auto ts_at = [&](uint64_t logical, uint64_t limit, uint64_t* ts) {
        Sample s{};
        for (uint64_t k = logical; k <= limit; ++k) {
            if (read(k, &s)) { *ts = s.timestamp_nanos; return true; }
        }
        return false;
    };

    // First index with ts >= since_ns.
    uint64_t lo = oldest, hi = newest + 1;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        uint64_t ts = 0;
        if (!ts_at(mid, newest, &ts) || ts >= since_ns) hi = mid;
        else                                             lo = mid + 1;
    }
    const uint64_t first = lo;
    // One past the last index with ts <= until_ns.
    uint64_t end = newest + 1;
    if (until_ns != 0) {
        lo = first; hi = newest + 1;
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo) / 2;
            uint64_t ts = 0;
            if (!ts_at(mid, newest, &ts) || ts > until_ns) hi = mid;
            else                                            lo = mid + 1;
        }
        end = lo;
    }
    if (first >= end) return 0;

    const uint64_t count = end - first;
    const uint64_t take  = count < max_records ? count : max_records;
    int added = 0;
    uint64_t prev = UINT64_MAX;
    for (uint64_t k = 0; k < take; ++k) {
        const uint64_t idx = take == 1 ? first
                           : first + (k * (count - 1)) / (take - 1);
        if (idx == prev) continue;
        prev = idx;
        Sample s{};
        if (!read(idx, &s)) continue;
        if (s.timestamp_nanos < since_ns) continue;
        if (until_ns != 0 && s.timestamp_nanos > until_ns) continue;
        if (override_level > 0) s.level = static_cast<Level>(override_level);
        out->push_back(s);
        ++added;
    }
    return added;
}

int TierManager::query_ring(const RingFile* ring, int override_level,
                            uint64_t since_ns, uint64_t until_ns,
                            std::size_t max_records, std::vector<Sample>* out) const {
    const uint64_t cap  = ring->capacity();
    const uint64_t widx = ring->write_index();
    if (cap == 0 || widx == 0) return 0;

    const uint64_t valid = widx < cap ? widx : cap;   // records still live

    // Walk newest → oldest so the time-window cutoff lets us stop early
    // and so an over-long window keeps the *newest* max_records. Records
    // are appended in timestamp order, so a sample older than since_ns
    // means every remaining one is too — break.
    const std::size_t rsize = record_size_for_sample();
    std::vector<uint8_t> buf(rsize);
    std::vector<Sample>  newest_first;
    newest_first.reserve(std::min<std::size_t>(max_records, valid));

    for (uint64_t k = 0; k < valid && newest_first.size() < max_records; ++k) {
        const uint64_t logical = widx - 1 - k;
        const uint64_t slot    = logical % cap;
        if (ring->read_at(slot, buf.data(), rsize) != 0) continue;
        Sample s{};
        if (record_decode(buf.data(), rsize, &s) != 0) continue;   // torn / CRC
        if (s.timestamp_nanos < since_ns) break;                   // window floor
        if (until_ns != 0 && s.timestamp_nanos > until_ns) continue;
        if (override_level > 0) s.level = static_cast<Level>(override_level);
        newest_first.push_back(s);
    }

    // Flip to chronological (oldest-first) for the caller.
    out->reserve(out->size() + newest_first.size());
    for (auto it = newest_first.rbegin(); it != newest_first.rend(); ++it) {
        out->push_back(*it);
    }
    return static_cast<int>(newest_first.size());
}

uint64_t TierManager::tier1_count() const { return tier1_.count(); }
uint64_t TierManager::tier2_count() const { return tier2_.count(); }
uint64_t TierManager::tier3_count() const { return tier3_.count(); }

} // namespace budyk
