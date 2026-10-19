// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/metric_set.h"
#include "core/sample.h"
#include "core/sample_c.h"

#include <memory>

namespace budyk {

/// What the stateful collectors (rates from counter deltas) keep across
/// ticks. Zero-initialise it before the first tick.
struct CollectContext {
    budyk_cpu_ctx_c     cpu{};
    budyk_disk_ctx_c    disk{};
    budyk_net_ctx_c     net{};
    budyk_netstat_ctx_c netstat{};
    /// Each core, disk, interface and filesystem, and their previous
    /// counters (~120 KB, hence the heap).
    std::unique_ptr<budyk_detail_c> detail{new budyk_detail_c()};
    /// The groups and lists read on the previous tick (MetricGroup /
    /// MetricDetail bits). A rate is the change between two reads in a
    /// row; one not read then has only a baseline now.
    uint32_t last_groups = 0;
    uint32_t last_detail = 0;
};

/// One collection tick: runs the platform collectors (plain C, chosen by
/// BUDYK_LINUX / BUDYK_FREEBSD) that `set` asks for, plus those of the
/// minimal set, into the C-shim sample and copies the result into `s`;
/// ctx->detail gets the lists `set` asks for. A list brings its group.
///
/// s->groups and ctx->detail->collected then say what this tick has:
/// what was read (file_watch is the serve loop's to add), and not the rates
/// read for the first time in a row: cpu, disk, net, tcp, udp, icmp and the
/// cores, disks, interfaces and processes lists, whose first read is a
/// baseline (on the first tick, or after a level that did not read them).
/// A group the
/// sample lacks is zeroed (clear_groups), a list it lacks is empty;
/// s->baseline and ctx->detail->baseline name those first reads.
void collect_one(Sample* s, CollectContext* ctx, const MetricSet& set = MetricSet{});

} // namespace budyk
