// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include <cstddef>
#include <cstdint>

namespace budyk {

// Circular buffer of the newest samples, for the WebSocket catch-up and
// /api/samples. Never written to disk. Not synchronised: the daemon
// pushes from the collector tick and reads from the HTTP thread, both
// under one mutex (src/daemon/serve.cpp).
class HotBuffer {
public:
    // capacity 0 is treated as 1: push() works modulo the capacity.
    explicit HotBuffer(size_t capacity = 300);
    ~HotBuffer();
    // Owns a raw array; a copy would double-free it.
    HotBuffer(const HotBuffer&)            = delete;
    HotBuffer& operator=(const HotBuffer&) = delete;

    void push(const Sample& s);

    // Dump all buffered samples into `out` (oldest first).
    // Returns number of samples written.
    size_t dump(Sample* out, size_t out_cap) const;

    void reset();
    size_t size() const;

private:
    Sample* ring_;
    size_t  capacity_;
    size_t  head_;
    size_t  count_;
};

// hot_buffer.warm_grace (spec §3.4, M4.2): once no client has been
// connected for the grace period, the hot buffer is emptied, so the next
// client's catch-up holds samples from after it rather than a 1 Hz burst
// from a session that ended long ago, glued to sparse L1 samples.
//
// Call should_reset() once per tick with the number of connected clients
// (WebSocket clients plus a recent /api/samples poller). It is true at
// most once per idle spell, and never before the first client: until
// then the buffer only holds samples taken since start-up. Times are the
// samples' realtime timestamps; a clock that steps backwards re-anchors
// the spell instead of expiring it.
class WarmGrace {
public:
    explicit WarmGrace(uint64_t grace_ns) : grace_ns_(grace_ns) {}

    bool should_reset(int clients, uint64_t now_ns);

    // Nanoseconds until should_reset() would return true, or UINT64_MAX
    // when no reset is pending. The serve loop caps its sleep with this,
    // so an L1 sleep of minutes does not delay the reset.
    uint64_t ns_until_due(uint64_t now_ns) const;

private:
    uint64_t grace_ns_;
    uint64_t last_client_ns_ = 0;
    bool     pending_        = false;
};

} // namespace budyk
