// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include <cstddef>
#include <cstdint>

namespace budyk {

/// Circular buffer of the newest samples, for the WebSocket catch-up and
/// /api/samples. Never written to disk. Not synchronised: the daemon
/// pushes from the collector tick and reads from the HTTP thread, both
/// under one mutex (src/daemon/serve.cpp).
class HotBuffer {
public:
    /// capacity 0 is treated as 1: push() works modulo the capacity.
    explicit HotBuffer(size_t capacity = 300);
    ~HotBuffer();
    /// Owns a raw array; a copy would double-free it.
    HotBuffer(const HotBuffer&)            = delete;
    HotBuffer& operator=(const HotBuffer&) = delete;

    void push(const Sample& s);

    /// Dump all buffered samples into `out` (oldest first).
    /// Returns number of samples written.
    size_t dump(Sample* out, size_t out_cap) const;

    /// The newest `n` samples (or all, if fewer) into `out`, oldest first.
    /// Returns the number written.
    size_t dump_last(Sample* out, size_t n) const;

    void reset();
    size_t size() const;

private:
    Sample* ring_;
    size_t  capacity_;
    size_t  head_;
    size_t  count_;
};

/// hot_buffer.warm_grace (spec §3.4, M4.2): once no client has been
/// connected for the grace period, the hot buffer is emptied, so the next
/// client's catch-up holds samples from after it rather than a 1 Hz burst
/// from a session that ended long ago, glued to sparse L1 samples.
///
/// Call should_reset() once per tick with the number of connected clients
/// (WebSocket clients plus a recent /api/samples poller) and whether the
/// tick is a fast one without them (L3 or a custom level: an escalation,
/// escalate(), a level's `when`), which fills the buffer as a session
/// does. It is true at most once per idle spell, and never before the
/// first client or fast tick: until then the buffer only holds samples
/// taken since start-up. Times are the samples' realtime timestamps; a
/// clock that steps backwards re-anchors the spell instead of expiring
/// it.
class WarmGrace {
public:
    explicit WarmGrace(uint64_t grace_ns) : grace_ns_(grace_ns) {}

    bool should_reset(int clients, uint64_t now_ns, bool fast = false);

    /// Nanoseconds until should_reset() would return true, or UINT64_MAX
    /// when no reset is pending or the last should_reset() saw a client.
    /// The serve loop caps its sleep with this, so an L1 sleep of minutes
    /// does not delay the reset.
    uint64_t ns_until_due(uint64_t now_ns) const;

    /// `sleep_ms` capped at the time until the reset is due, no lower than
    /// `floor_ms`: the reset due moments after a tick ended made the next
    /// tick a few ms after it, and its rates, measured over those ms, were
    /// stored (a 0 % or 100 % CPU in the history, an anomaly run from it).
    int cap_sleep_ms(int sleep_ms, uint64_t now_ns, int floor_ms) const;

private:
    uint64_t grace_ns_;
    uint64_t last_client_ns_ = 0;
    bool     pending_        = false;
    bool     connected_      = false;   ///< the last should_reset() saw a client
};

} // namespace budyk
