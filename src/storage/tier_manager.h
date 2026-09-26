// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include "storage/ring_file.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace budyk {

// Multi-tier coordinator: routes encoded samples to one of three on-disk
// ring buffers based on Sample::level.
//
//   tier1_  ←  L3 samples (raw, highest cadence)
//   tier2_  ←  L2 samples (1-minute aggregates produced by TierAggregator)
//   tier3_  ←  L1 samples (5-minute aggregates produced by TierAggregator)
//
// TierManager itself does not aggregate — that's TierAggregator's job. The
// daemon main loop drives the cadence, calling store() once per sample
// regardless of tier.
// One user-defined level's ring (collection.levels). The file is named
// after the level (level-<name>.ring), not its id, so reordering the
// config doesn't point a level at another level's history.
struct LevelRingSpec {
    uint8_t     level_id = 0;
    std::string name;
    int         max_mb   = 50;
};

class TierManager {
public:
    // Capacities are expressed in MiB; converted to record counts using
    // record_size_for_sample().
    int  init(const char* data_dir,
              int tier1_max_mb = 250,
              int tier2_max_mb = 150,
              int tier3_max_mb = 50,
              const std::vector<LevelRingSpec>& custom = {});

    // Encodes `s` and appends it to the ring matching its level.
    // Returns 0 on success, negative on encode/write failure or if
    // the manager is not initialised. Unknown levels are rejected.
    int  store(const Sample& s);

    void close();

    // Read decoded samples from one tier's ring whose timestamp_nanos
    // falls in [since_ns, until_ns]. tier is 1 (raw L3), 2 (1-min L2),
    // or 3 (5-min L1). until_ns == 0 means "no upper bound" (i.e. now).
    // Results are appended to *out oldest-first, capped at max_records
    // (the newest are kept when the window holds more). Returns the
    // number appended, or -1 on bad args / not initialised. Records
    // that fail to decode — a torn read racing the collector thread, or
    // CRC skew — are silently skipped, same robustness model as the
    // ring itself. Safe to call from a different thread than store():
    // both go through pread/pwrite + an atomic write_idx.
    int query(int tier, uint64_t since_ns, uint64_t until_ns,
              std::size_t max_records, std::vector<Sample>* out) const;

    // Same, by level: L3 → tier 1, L2 → tier 2, L1 → tier 3, a custom
    // level → its own ring. Samples read from a custom ring carry that
    // level's current id, whatever id was current when they were written.
    int query_level(Level level, uint64_t since_ns, uint64_t until_ns,
                    std::size_t max_records, std::vector<Sample>* out) const;

    // Every ring, merged oldest-first: what was collected in the window
    // at whatever level. Each ring contributes at most max_records spread
    // evenly over its part of the window. If the merge holds more than
    // max_records, it's thinned by time — the window is cut into
    // max_records equal slices and the newest sample of each slice is
    // kept — so a long window stays covered end to end.
    int query_all(uint64_t since_ns, uint64_t until_ns,
                  std::size_t max_records, std::vector<Sample>* out) const;

    uint64_t tier1_count() const;
    uint64_t tier2_count() const;
    uint64_t tier3_count() const;

private:
    struct CustomRing {
        uint8_t                   level_id;
        std::unique_ptr<RingFile> ring;
    };

    RingFile tier1_, tier2_, tier3_;
    std::vector<CustomRing> custom_;
    bool     ready_ = false;

    const RingFile* ring_for(Level level) const;
    int query_ring(const RingFile* ring, int override_level,
                   uint64_t since_ns, uint64_t until_ns,
                   std::size_t max_records, std::vector<Sample>* out) const;
    // Like query_ring, but when the window holds more than max_records it
    // reads max_records records evenly spread across the window instead
    // of the newest ones. The window's bounds are found by binary search
    // over the ring (records are in time order), so the cost is
    // O(log n + max_records) reads whatever the window size.
    int query_ring_spread(const RingFile* ring, int override_level,
                          uint64_t since_ns, uint64_t until_ns,
                          std::size_t max_records, std::vector<Sample>* out) const;
};

} // namespace budyk
