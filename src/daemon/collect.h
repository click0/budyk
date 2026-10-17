// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include "core/sample_c.h"

namespace budyk {

// What the stateful collectors (rates from counter deltas) keep across
// ticks. Zero-initialise it before the first tick.
struct CollectContext {
    budyk_cpu_ctx_c     cpu{};
    budyk_disk_ctx_c    disk{};
    budyk_net_ctx_c     net{};
    budyk_netstat_ctx_c netstat{};
};

// One collection tick: runs every platform collector (plain C, chosen
// by BUDYK_LINUX / BUDYK_FREEBSD) into the C-shim sample and copies the
// result into `s`, with `s->groups` set to the groups collected.
void collect_one(Sample* s, CollectContext* ctx);

} // namespace budyk
