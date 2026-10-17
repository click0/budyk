// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

#include <cstdint>
#include <string>
#include <vector>

namespace budyk {

// The per-entity collections (budyk_detail_c), as bits of
// MetricSet::detail and budyk_detail_c::collected.
enum MetricDetail : uint32_t {
    kDetailCores      = 1u << 0,   // cpu.cores
    kDetailDisks      = 1u << 1,   // disk.devices
    kDetailInterfaces = 1u << 2,   // net.interfaces
    kDetailMounts     = 1u << 3,   // fs.mounts
};
constexpr uint32_t kDetailAll = (1u << 4) - 1;

// What the scheduler's anomaly check reads, and spec §3.4.1's L1 set:
// every level collects these.
constexpr uint32_t kGroupsMinimal =
    kGroupCpu | kGroupMem | kGroupSwap | kGroupLoad | kGroupUptime;
// The groups a level can be told to collect: all but file_watch.
constexpr uint32_t kGroupsSelectable = kGroupsAll & ~static_cast<uint32_t>(kGroupFileWatch);

// What one collection level reads on each tick (collection.<level>.metrics).
// file_watch is not in it: the watcher, when enabled, is drained on every
// tick whatever the level.
struct MetricSet {
    uint32_t groups = kGroupsSelectable;   // MetricGroup bits
    uint32_t detail = kDetailAll;          // MetricDetail bits
};

// The named sets:
//   minimal   cpu, mem, swap, load, uptime
//   standard  every group (the totals), no per-entity detail
//   full      standard and the detail: cpu.cores, disk.devices,
//             net.interfaces, fs.mounts
MetricSet metric_set_minimal();
MetricSet metric_set_standard();
MetricSet metric_set_full();

// Parses a set from the names given: set names (above), group names
// ("cpu", "mem", "swap", "load", "uptime", "disk", "net", "proc",
// "entropy", "self", "thermal", "tcp", "udp", "fs", "clock") and
// collection names ("cpu.cores", "disk.devices",
// "net.interfaces", "fs.mounts"), case-sensitive. The result is their
// union plus the minimal set; a collection brings its group with it.
// Returns 0, or -EINVAL with *error naming the first unknown name (*out
// is then unchanged).
int parse_metric_set(const std::vector<std::string>& names, MetricSet* out,
                     std::string* error);

// The name of one MetricGroup / MetricDetail bit ("cpu", "file_watch",
// "cpu.cores"), or nullptr for anything that is not exactly one known bit.
const char* metric_group_name(uint32_t group_bit);
const char* metric_detail_name(uint32_t detail_bit);

// The bit of a group / collection name, or 0 if there is none.
uint32_t metric_group_bit(const char* name);
uint32_t metric_detail_bit(const char* name);

// Every name in `set`, groups first, in bit order:
// "cpu", "mem", ..., "cpu.cores", ...
std::vector<std::string> metric_set_names(const MetricSet& set);

} // namespace budyk
