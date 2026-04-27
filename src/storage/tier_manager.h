// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include "storage/ring_file.h"

#include <cstdint>

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
class TierManager {
public:
    // Capacities are expressed in MiB; converted to record counts using
    // record_size_for_sample().
    int  init(const char* data_dir,
              int tier1_max_mb = 250,
              int tier2_max_mb = 150,
              int tier3_max_mb = 50);

    // Encodes `s` and appends it to the ring matching its level.
    // Returns 0 on success, negative on encode/write failure or if
    // the manager is not initialised. Unknown levels are rejected.
    int  store(const Sample& s);

    void close();

    uint64_t tier1_count() const;
    uint64_t tier2_count() const;
    uint64_t tier3_count() const;

private:
    RingFile tier1_, tier2_, tier3_;
    bool     ready_ = false;
};

} // namespace budyk
