// SPDX-License-Identifier: BSD-3-Clause
#include "storage/tier_manager.h"

#include "storage/codec.h"
#include "storage/ring_check.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace budyk {

namespace {

// Capacities arrive in MiB; convert to record counts using the codec's
// per-record size. Floor at 1 record so an absurdly tiny budget still
// produces a usable ring (mostly for tests).
uint64_t records_from_mb(int mb, size_t record_size) {
    if (mb <= 0 || record_size == 0) return 1;
    constexpr uint64_t kMiB  = 1024 * 1024;
    const uint64_t bytes = static_cast<uint64_t>(mb) * kMiB;
    const uint64_t cap   = bytes / record_size;
    return cap == 0 ? 1 : cap;
}

bool join_path(char* out, size_t cap, const char* dir, const char* leaf) {
    int n = std::snprintf(out, cap, "%s/%s", dir, leaf);
    return n > 0 && static_cast<size_t>(n) < cap;
}

// Time-ordered access to one ring by logical index (write order).
class RingReader {
public:
    explicit RingReader(const RingFile* ring)
        : ring_(ring), cap_(ring->capacity()), buf_(record_size_for_sample()) {
        const uint64_t widx  = ring->write_index();
        const uint64_t valid = widx < cap_ ? widx : cap_;
        oldest_ = widx - valid;
        end_    = widx;
    }

    bool     empty()  const { return cap_ == 0 || end_ == oldest_; }
    uint64_t oldest() const { return oldest_; }
    uint64_t end()    const { return end_; }

    bool read(uint64_t logical, Sample* s) {
        return ring_->read_at(logical % cap_, buf_.data(), buf_.size()) == 0 &&
               record_decode(buf_.data(), buf_.size(), s) == 0;
    }

    // First logical index whose timestamp is >= ts (end() if none). A
    // record that doesn't decode (e.g. the one being written) counts as
    // the next one that does. Records are in time order, so this is a
    // binary search: O(log n) reads.
    uint64_t lower_bound(uint64_t ts) {
        uint64_t lo = oldest_, hi = end_;
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo) / 2;
            uint64_t t = 0;
            if (!ts_from(mid, &t) || t >= ts) hi = mid;
            else                              lo = mid + 1;
        }
        return lo;
    }

    // True if the ring holds a record with lo < ts <= hi.
    bool any_in(uint64_t lo, uint64_t hi) {
        if (empty() || hi <= lo) return false;
        uint64_t ts = 0;
        return ts_from(lower_bound(lo + 1), &ts) && ts <= hi;
    }

    // Timestamp at `logical`, stepping forward past records that don't
    // decode. False if none decodes up to the end.
    bool ts_from(uint64_t logical, uint64_t* ts) {
        Sample s{};
        for (uint64_t k = logical; k < end_; ++k) {
            if (read(k, &s)) { *ts = s.timestamp_nanos; return true; }
        }
        return false;
    }

private:
    const RingFile*      ring_;
    uint64_t             cap_;
    uint64_t             oldest_ = 0;
    uint64_t             end_    = 0;
    std::vector<uint8_t> buf_;
};

// Drops from `recs` the folded records whose fold window (the `window_ns`
// up to the record's timestamp) still has samples in one of the finer
// `rings`. `seen` holds what was already read from those rings, so most
// checks need no I/O; the rest are a binary search per ring.
void drop_covered(std::vector<Sample>* recs, uint64_t window_ns,
                  const std::vector<const std::vector<Sample>*>& seen,
                  const std::vector<const RingFile*>& rings) {
    if (window_ns == 0) return;   // folding off: nothing to compare with
    std::vector<uint64_t> seen_ts;
    for (const auto* v : seen) {
        for (const auto& s : *v) seen_ts.push_back(s.timestamp_nanos);
    }
    std::sort(seen_ts.begin(), seen_ts.end());
    std::vector<RingReader> readers;
    readers.reserve(rings.size());
    for (const auto* r : rings) readers.emplace_back(r);

    recs->erase(std::remove_if(recs->begin(), recs->end(), [&](const Sample& s) {
        if (!s.folded) return false;
        const uint64_t hi = s.timestamp_nanos;
        const uint64_t lo = hi > window_ns ? hi - window_ns : 0;
        const auto it = std::upper_bound(seen_ts.begin(), seen_ts.end(), lo);
        if (it != seen_ts.end() && *it <= hi) return true;
        for (auto& r : readers) {
            if (r.any_in(lo, hi)) return true;
        }
        return false;
    }), recs->end());
}

} // namespace

int TierManager::init(const char* data_dir,
                      int tier1_max_mb,
                      int tier2_max_mb,
                      int tier3_max_mb,
                      const std::vector<LevelRingSpec>& custom) {
    last_error_.clear();
    recovered_.clear();
    converted_.clear();
    if (data_dir == nullptr) { last_error_ = "data_dir is null"; return -1; }
    if (ready_)              { last_error_ = "already initialised"; return -2; }

    // Records why a ring failed to open, for the operator's log line.
    auto ring_failed = [this](const char* leaf, const RingFile& ring, int rc) {
        last_error_  = leaf;
        last_error_ += ": ";
        last_error_ += RingFile::describe(rc);
        if (ring.last_errno() != 0) {
            last_error_ += ": ";
            last_error_ += std::strerror(ring.last_errno());
        }
        if (rc == -6 || rc == -11 || rc == -12) {
            last_error_ += " (storage_mb / tierN_max_mb changed? move the file "
                           "aside or restore the old size)";
        }
    };
    auto path_failed = [this](const char* leaf) {
        last_error_  = leaf;
        last_error_ += ": data_dir path too long";
    };

    const uint32_t record_size = static_cast<uint32_t>(record_size_for_sample());
    const uint64_t cap1 = records_from_mb(tier1_max_mb, record_size);
    const uint64_t cap2 = records_from_mb(tier2_max_mb, record_size);
    const uint64_t cap3 = records_from_mb(tier3_max_mb, record_size);

    char path[1024];

    int rc = 0;
    if (!join_path(path, sizeof(path), data_dir, "tier1.ring")) { path_failed("tier1.ring"); return -3; }
    if (!convert_if_older(path, "tier1.ring", 1, record_size, cap1)) return -4;
    if ((rc = tier1_.open(path, /*tier*/1, record_size, cap1)) != 0) {
        ring_failed("tier1.ring", tier1_, rc);
        return -4;
    }
    recover("tier1.ring", &tier1_);

    if (!join_path(path, sizeof(path), data_dir, "tier2.ring")) {
        path_failed("tier2.ring");
        tier1_.close();
        return -5;
    }
    if (!convert_if_older(path, "tier2.ring", 2, record_size, cap2)) {
        tier1_.close();
        return -6;
    }
    if ((rc = tier2_.open(path, /*tier*/2, record_size, cap2)) != 0) {
        ring_failed("tier2.ring", tier2_, rc);
        tier1_.close();
        return -6;
    }
    recover("tier2.ring", &tier2_);

    if (!join_path(path, sizeof(path), data_dir, "tier3.ring")) {
        path_failed("tier3.ring");
        tier2_.close();
        tier1_.close();
        return -7;
    }
    if (!convert_if_older(path, "tier3.ring", 3, record_size, cap3)) {
        tier2_.close();
        tier1_.close();
        return -8;
    }
    if ((rc = tier3_.open(path, /*tier*/3, record_size, cap3)) != 0) {
        ring_failed("tier3.ring", tier3_, rc);
        tier2_.close();
        tier1_.close();
        return -8;
    }
    recover("tier3.ring", &tier3_);

    // Custom levels. Every custom ring has the same header tier byte; the
    // file name identifies the level.
    for (const auto& spec : custom) {
        const std::string leaf = "level-" + spec.name + ".ring";
        auto ring = std::unique_ptr<RingFile>(new RingFile());
        if (!join_path(path, sizeof(path), data_dir, leaf.c_str())) {
            path_failed(leaf.c_str());
            rc = -3;
        } else if (!convert_if_older(path, leaf.c_str(), kCustomRingTier, record_size,
                                     records_from_mb(spec.max_mb, record_size))) {
            rc = -3;
        } else {
            rc = ring->open(path, kCustomRingTier, record_size,
                            records_from_mb(spec.max_mb, record_size));
            if (rc != 0) ring_failed(leaf.c_str(), *ring, rc);
        }
        if (rc != 0) {
            for (auto& c : custom_) c.ring->close();
            custom_.clear();
            tier3_.close();
            tier2_.close();
            tier1_.close();
            return -9;
        }
        recover(leaf.c_str(), ring.get());
        custom_.push_back(CustomRing{spec.level_id, std::move(ring)});
    }

    ready_ = true;
    return 0;
}

const std::string& TierManager::last_error() const { return last_error_; }

const std::vector<TierManager::Recovered>& TierManager::recovered() const {
    return recovered_;
}

const std::vector<TierManager::Converted>& TierManager::converted() const {
    return converted_;
}

bool TierManager::convert_if_older(const char* path, const char* leaf, uint8_t tier,
                                   uint32_t record_size, uint64_t capacity) {
    RingFile::Header h;
    int err = 0;
    if (RingFile::read_header(path, &h, &err) != 0 || h.tier != tier ||
        h.record_size == record_size) {
        return true;   // nothing to convert; open() reports anything else
    }
    if (h.record_size > record_size) {
        last_error_  = leaf;
        last_error_ += ": ";
        last_error_ += std::to_string(h.record_size);
        last_error_ += "-byte records, written by a newer budyk (this one writes ";
        last_error_ += std::to_string(record_size);
        last_error_ += "): move the file aside, or run the newer budyk";
        return false;
    }
    uint64_t kept = 0, dropped = 0;
    const int rc = convert_ring(path, tier, record_size, capacity, &kept, &dropped);
    if (rc != 0) {
        last_error_  = leaf;
        last_error_ += ": converting it from ";
        last_error_ += std::to_string(h.record_size);
        last_error_ += "-byte records (an older budyk) failed: ";
        last_error_ += std::strerror(-rc);
        return false;
    }
    converted_.push_back(Converted{leaf, h.record_size, kept, dropped});
    return true;
}

void TierManager::recover(const char* leaf, RingFile* ring) {
    const uint64_t n = count_unindexed(*ring);
    if (n > 0 && ring->advance_write_index(n) == 0) {
        recovered_.push_back(Recovered{leaf, n});
    }
}

void TierManager::set_fold_windows(uint64_t l2_ns, uint64_t l1_ns) {
    fold_l2_ns_ = l2_ns;
    fold_l1_ns_ = l1_ns;
}

int TierManager::store(const Sample& s) {
    if (!ready_) return -1;

    // Pick the destination first — fail fast for unknown levels rather
    // than wasting an encode pass.
    RingFile* target = const_cast<RingFile*>(ring_for(s.level));
    if (target == nullptr) return -2;

    // Fold windows before the direct write: when an L2 or L1 sample
    // arrives, the window that was folding into its ring is written
    // first, so the ring stays in time order. The first failure wins.
    int rc = 0;
    auto keep = [&rc](int r) { if (rc == 0) rc = r; };
    if (s.level == Level::L1) {
        keep(flush(&fold_l2_, &tier2_));
        keep(flush(&fold_l1_, &tier3_));
    } else if (s.level == Level::L2) {
        keep(flush(&fold_l2_, &tier2_));
        keep(feed(&fold_l1_, &tier3_, fold_l1_ns_, s));
    } else {   // L3 or a custom level
        keep(feed(&fold_l2_, &tier2_, fold_l2_ns_, s));
        keep(feed(&fold_l1_, &tier3_, fold_l1_ns_, s));
    }
    const int direct = append(target, s);
    return direct != 0 ? direct : rc;
}

int TierManager::append(RingFile* ring, const Sample& s) {
    // Headroom over the current record size (14 + 528 = 542 for codec
    // v9); record_encode() refuses rather than overflow if it ever grows
    // past this.
    uint8_t buf[1024];
    size_t  len = 0;
    if (record_encode(s, buf, sizeof(buf), &len) != 0) return -3;
    if (ring->append(buf, len) != 0)                   return -4;
    return 0;
}

int TierManager::feed(TierAggregator* agg, RingFile* ring, uint64_t window_ns,
                      const Sample& s) {
    if (window_ns == 0) return 0;
    int rc = 0;
    if (agg->size() > 0) {
        const uint64_t first = agg->first_timestamp();
        if (s.timestamp_nanos < first || s.timestamp_nanos - first >= window_ns) {
            rc = flush(agg, ring);
        }
    }
    agg->add(s);
    return rc;
}

int TierManager::flush(TierAggregator* agg, RingFile* ring) {
    Sample folded{};
    if (!agg->fold(&folded)) return 0;   // empty window
    return append(ring, folded);
}

void TierManager::close() {
    if (!ready_) return;
    flush(&fold_l2_, &tier2_);
    flush(&fold_l1_, &tier3_);
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

    // Each ring's share, kept apart until the folded records are checked.
    std::vector<Sample> fine, l2, l1;   // fine: L3 and the custom levels
    std::vector<const RingFile*> finer_than_l2 = {&tier1_};
    query_ring_spread(&tier1_, -1, since_ns, until_ns, max_records, &fine);
    for (const auto& c : custom_) {
        query_ring_spread(c.ring.get(), c.level_id, since_ns, until_ns,
                          max_records, &fine);
        finer_than_l2.push_back(c.ring.get());
    }
    query_ring_spread(&tier2_, -1, since_ns, until_ns, max_records, &l2);
    query_ring_spread(&tier3_, -1, since_ns, until_ns, max_records, &l1);

    // A folded record repeats what a finer ring holds: keep it only where
    // that ring no longer does. L1 first, while l2 still has its folded
    // records: they show finer data was collected there too, and seen in
    // memory they save a search of tier2.
    std::vector<const RingFile*> finer_than_l1 = finer_than_l2;
    finer_than_l1.push_back(&tier2_);
    drop_covered(&l1, fold_l1_ns_, {&fine, &l2}, finer_than_l1);
    drop_covered(&l2, fold_l2_ns_, {&fine}, finer_than_l2);

    std::vector<Sample> merged;
    merged.reserve(fine.size() + l2.size() + l1.size());
    merged.insert(merged.end(), fine.begin(), fine.end());
    merged.insert(merged.end(), l2.begin(), l2.end());
    merged.insert(merged.end(), l1.begin(), l1.end());
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
    RingReader r(ring);
    if (r.empty()) return 0;

    // [first, end): the records with since_ns <= ts <= until_ns.
    const uint64_t first = r.lower_bound(since_ns);
    const uint64_t end   = until_ns != 0 && until_ns != UINT64_MAX
                         ? r.lower_bound(until_ns + 1) : r.end();
    if (first >= end) return 0;

    const uint64_t count = end - first;
    const uint64_t take  = count < max_records ? count : max_records;
    int added = 0;
    uint64_t prev = UINT64_MAX;
    for (uint64_t k = 0; k < take; ++k) {
        // Evenly spread, both ends included; one record means the newest,
        // as everywhere else a limit applies.
        const uint64_t idx = take == 1 ? end - 1
                           : first + (k * (count - 1)) / (take - 1);
        if (idx == prev) continue;
        prev = idx;
        Sample s{};
        if (!r.read(idx, &s)) continue;
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
