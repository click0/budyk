// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include "storage/ring_file.h"

namespace budyk {

// Multi-tier coordinator: Tier1 (raw), Tier2 (1m agg), Tier3 (5m agg).
class TierManager {
public:
    int  init(const char* data_dir);
    void store(const Sample& s);
    void close();
    // TODO: query API for /api/history
private:
    RingFile tier1_, tier2_, tier3_;
};

} // namespace budyk
