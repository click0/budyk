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
                      int tier3_max_mb) {
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

    ready_ = true;
    return 0;
}

int TierManager::store(const Sample& s) {
    if (!ready_) return -1;

    // Pick the destination first — fail fast for unknown levels rather
    // than wasting an encode pass.
    RingFile* target = nullptr;
    switch (s.level) {
        case Level::L3: target = &tier1_; break;   // raw
        case Level::L2: target = &tier2_; break;   // 1-minute aggregate
        case Level::L1: target = &tier3_; break;   // 5-minute aggregate
    }
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
    tier3_.close();
    tier2_.close();
    tier1_.close();
    ready_ = false;
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
