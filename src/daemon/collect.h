// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/metric_set.h"
#include "core/sample.h"
#include "core/sample_c.h"

#include <memory>

namespace budyk {

/// CollectContext::cut bits besides the MetricDetail ones: the disk and
/// the network totals followed BUDYK_MAX_COUNTED devices and there were more.
constexpr uint32_t kCutDiskTotals = 1u << 16;
constexpr uint32_t kCutNetTotals  = 1u << 17;

/// What the stateful collectors (rates from counter deltas) keep across
/// ticks. Zero-initialise it before the first tick.
struct CollectContext {
    budyk_cpu_ctx_c     cpu{};
    budyk_disk_ctx_c    disk{};
    budyk_net_ctx_c     net{};
    budyk_netstat_ctx_c netstat{};
    /// Each core, disk, interface and filesystem, and their previous
    /// counters (~710 KB, hence the heap).
    std::unique_ptr<budyk_detail_c> detail{new budyk_detail_c()};
    /// The groups and lists read on the previous tick (MetricGroup /
    /// MetricDetail bits). A rate is the change between two reads in a
    /// row; one not read then has only a baseline now.
    uint32_t last_groups = 0;
    uint32_t last_detail = 0;
    /// The groups whose collector failed on the last call: not in
    /// s->groups, and a baseline when they read again.
    uint32_t failed = 0;
    /// What was cut short when last checked, logged when it starts: a
    /// list holds BUDYK_MAX_ENTITIES, the totals follow BUDYK_MAX_COUNTED
    /// devices of each kind (bits: kCutDiskTotals, kCutNetTotals, and the
    /// MetricDetail bit of the list). A call that does not read a list or
    /// a group leaves its bit as it was.
    uint32_t cut = 0;
};

/// One collection tick: runs the platform collectors (plain C, chosen by
/// BUDYK_LINUX / BUDYK_FREEBSD) that `set` asks for, plus those of the
/// minimal set, into the C-shim sample and copies the result into `s`;
/// ctx->detail gets the lists `set` asks for. A list brings its group.
///
/// s->groups and ctx->detail->collected then say what this tick has:
/// what was read (file_watch is the serve loop's to add; a collector that
/// failed is left out, logged when it starts failing), and not the rates
/// read for the first time in a row: cpu, disk, net, tcp, udp, icmp and the
/// cores, disks, interfaces and processes lists, whose first read is a
/// baseline (on the first tick, or after a level that did not read them).
/// A group the
/// sample lacks is zeroed (clear_groups), a list it lacks is empty;
/// s->baseline and ctx->detail->baseline name those first reads.
void collect_one(Sample* s, CollectContext* ctx, const MetricSet& set = MetricSet{});

} // namespace budyk
