// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "storage/ring_file.h"

#include <cstdint>
#include <vector>

namespace budyk {

// Records past `ring`'s write_idx that were written but never counted:
// a crash between a record's pwrite and the write_idx update leaves one
// (a lost header page, more). They are the consecutive slots after the
// newest indexed record that decode, carry the ring's level and are
// newer than the record before; in a wrapped ring the oldest record ends
// the run. TierManager::init() indexes them on start, fsck --repair too.
uint64_t count_unindexed(const RingFile& ring);

// True if a record of level `level` belongs in a ring with header tier
// byte `tier`: 1 holds L3, 2 L2, 3 L1, kCustomRingTier a custom level.
bool level_fits_ring(uint8_t tier, uint8_t level);

// What budyk fsck found in one ring file.
struct RingCheck {
    // The header (RingFile::read_header): 0, or why it is unusable, in
    // which case nothing else is filled in.
    int      header_rc    = 0;
    int      header_errno = 0;
    RingFile::Header header;
    bool     unknown_tier  = false;   // tier byte not 1, 2, 3 or custom
    bool     record_size_differs = false;   // written by a budyk with
                                            // another record format
    int      open_rc       = 0;       // RingFile open, after the header

    // The records write_idx counts.
    uint64_t records      = 0;
    uint64_t valid        = 0;
    uint64_t folded       = 0;
    uint64_t damaged      = 0;        // CRC, payload or level byte bad
    uint64_t wrong_level  = 0;        // a level that is not this ring's
    uint64_t out_of_order = 0;        // older than a record before it
    uint64_t future       = 0;        // more than a day ahead of now
    uint64_t read_errors  = 0;        // pread failed
    uint64_t first_ts     = 0;        // oldest and newest valid record
    uint64_t last_ts      = 0;
    std::vector<uint64_t> damaged_slots;   // the first few

    // Written past write_idx and never counted; indexed with repair.
    uint64_t unindexed    = 0;
    uint64_t recovered    = 0;

    // Problems that are not repaired by this run (see budyk.8, fsck).
    bool errors() const;
};

// Checks the ring file at `path` against `now_ns`. With `repair`, the
// file is opened for writing and the unindexed records are counted in
// (the only repair: damaged records are already skipped on read, and an
// out-of-order stretch is real data after a clock step). The caller
// holds the data_dir lock for a repair. Returns 0 (see *out), or -errno
// if the file cannot be opened for the repair.
int check_ring(const char* path, bool repair, uint64_t now_ns, RingCheck* out);

} // namespace budyk
