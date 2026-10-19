// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include "storage/ring_file.h"
#include "storage/tier_aggregator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace budyk {

/// Multi-tier coordinator: routes encoded samples to one of three on-disk
/// ring buffers based on Sample::level, plus one ring per custom level.
///
///   tier1_  ←  L3 samples (raw, highest cadence)
///   tier2_  ←  L2 samples, and L2 records folded from L3 / custom samples
///   tier3_  ←  L1 samples, and L1 records folded from everything finer
///
/// The daemon main loop calls store() once per sample whatever its level;
/// with fold windows set (set_fold_windows), store() also feeds the two
/// TierAggregators, so the L2 and L1 rings keep their cadence while a
/// finer level collects (spec §3.4.1).
/// One user-defined level's ring (collection.levels). The file is named
/// after the level (`level-<name>.ring`), not its id, so reordering the
/// config doesn't point a level at another level's history.
struct LevelRingSpec {
    uint8_t     level_id = 0;
    std::string name;
    int         max_mb   = 50;
};

class TierManager {
public:
    /// Capacities are expressed in MiB; converted to record counts using
    /// record_size_for_sample().
    int  init(const char* data_dir,
              int tier1_max_mb = 250,
              int tier2_max_mb = 150,
              int tier3_max_mb = 50,
              const std::vector<LevelRingSpec>& custom = {});

    /// After a failed init(): which ring file and why, in words, e.g.
    /// "tier1.ring: open failed: Permission denied" or "level-burst.ring:
    /// file size does not match record size x capacity (storage_mb
    /// changed? move the file aside or restore the old size)".
    const std::string& last_error() const;

    /// Records init() found written past a ring's write_idx and counted in
    /// (a crash between the record's pwrite and the index update; see
    /// count_unindexed()), per ring file, for the daemon's log.
    struct Recovered {
        std::string ring;      ///< "tier1.ring", "level-burst.ring"
        uint64_t    records = 0;
    };
    const std::vector<Recovered>& recovered() const;

    /// Rings init() found written by an older budyk (smaller records) and
    /// converted to this version's (convert_ring()), for the log.
    struct Converted {
        std::string ring;
        uint32_t    from_record_size = 0;
        uint64_t    kept    = 0;
        uint64_t    dropped = 0;
    };
    const std::vector<Converted>& converted() const;

    /// Fold windows for the coarse rings. With l2_ns set, every L3 or
    /// custom-level sample is also folded into one L2 record per l2_ns in
    /// tier2; with l1_ns set, every sample above L1 into one L1 record per
    /// l1_ns in tier3 (folded == true, see TierAggregator for the fields).
    /// A window ends once it has lasted its length, when a sample of the
    /// ring's own level arrives (the folded record goes first, so each
    /// ring stays in time order), or on close(). 0 turns folding off for
    /// that ring, the default. The daemon passes the L2 and L1 intervals.
    /// Call before the manager is shared with another thread.
    void set_fold_windows(uint64_t l2_ns, uint64_t l1_ns);

    /// Encodes `s` and appends it to the ring matching its level, then
    /// feeds the fold windows. Returns 0 on success, negative on
    /// encode/write failure (of the sample or a folded record) or if the
    /// manager is not initialised. Unknown levels are rejected.
    int  store(const Sample& s);

    /// Writes the open fold windows (a partial window is still a summary
    /// of what was collected) and closes the rings.
    void close();

    /// Read decoded samples from one tier's ring whose timestamp_nanos
    /// falls in [since_ns, until_ns]. tier is 1 (raw L3), 2 (1-min L2),
    /// or 3 (5-min L1). until_ns == 0 means "no upper bound" (i.e. now).
    /// Results are appended to *out oldest-first, capped at max_records
    /// (the newest are kept when the window holds more). Returns the
    /// number appended, or -1 on bad args / not initialised. Records
    /// that fail to decode — a torn read racing the collector thread, or
    /// CRC skew — are silently skipped, same robustness model as the
    /// ring itself. Safe to call from a different thread than store():
    /// both go through pread/pwrite + an atomic write_idx.
    int query(int tier, uint64_t since_ns, uint64_t until_ns,
              std::size_t max_records, std::vector<Sample>* out) const;

    /// Same, by level: L3 → tier 1, L2 → tier 2, L1 → tier 3, a custom
    /// level → its own ring. Samples read from a custom ring carry that
    /// level's current id, whatever id was current when they were written.
    int query_level(Level level, uint64_t since_ns, uint64_t until_ns,
                    std::size_t max_records, std::vector<Sample>* out) const;

    /// Every ring, merged oldest-first: what was collected in the window
    /// at whatever level. Each ring contributes at most max_records spread
    /// evenly over its part of the window. A folded record is left out
    /// where a finer ring still holds samples from its fold window, so a
    /// stretch of L3 shows once, at L3; where the finer samples are gone
    /// (that ring has wrapped), the folded record stands in for them. If
    /// the merge holds more than max_records, it's thinned by time — the
    /// window is cut into max_records equal slices and the newest sample
    /// of each slice is kept — so a long window stays covered end to end.
    int query_all(uint64_t since_ns, uint64_t until_ns,
                  std::size_t max_records, std::vector<Sample>* out) const;

    uint64_t tier1_count() const;
    uint64_t tier2_count() const;
    uint64_t tier3_count() const;

private:
    std::string last_error_;
    std::vector<Recovered> recovered_;
    std::vector<Converted> converted_;
    void recover(const char* leaf, RingFile* ring);
    /// Converts the ring at `path` first if an older budyk wrote it;
    /// false (with last_error_ set) if that failed.
    bool convert_if_older(const char* path, const char* leaf, uint8_t tier,
                          uint32_t record_size, uint64_t capacity);
    struct CustomRing {
        uint8_t                   level_id;
        std::unique_ptr<RingFile> ring;
    };

    RingFile tier1_, tier2_, tier3_;
    std::vector<CustomRing> custom_;
    bool     ready_ = false;

    uint64_t       fold_l2_ns_ = 0;
    uint64_t       fold_l1_ns_ = 0;
    TierAggregator fold_l2_{Level::L2};
    TierAggregator fold_l1_{Level::L1};

    const RingFile* ring_for(Level level) const;
    int  append(RingFile* ring, const Sample& s);
    /// Adds `s` to a fold window, first writing the window to `ring` if
    /// `s` falls past its end (or before its start: a clock step back).
    int  feed(TierAggregator* agg, RingFile* ring, uint64_t window_ns, const Sample& s);
    int  flush(TierAggregator* agg, RingFile* ring);
    int query_ring(const RingFile* ring, int override_level,
                   uint64_t since_ns, uint64_t until_ns,
                   std::size_t max_records, std::vector<Sample>* out) const;
    /// Like query_ring, but when the window holds more than max_records it
    /// reads max_records records evenly spread across the window instead
    /// of the newest ones; both ends are included, and max_records == 1
    /// gives the newest. The window's bounds are found by binary search
    /// over the ring (records are in time order), so the cost is
    /// O(log n + max_records) reads whatever the window size.
    int query_ring_spread(const RingFile* ring, int override_level,
                          uint64_t since_ns, uint64_t until_ns,
                          std::size_t max_records, std::vector<Sample>* out) const;
};

} // namespace budyk
