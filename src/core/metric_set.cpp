// SPDX-License-Identifier: BSD-3-Clause
#include "core/metric_set.h"

#include <cerrno>
#include <cstring>

namespace budyk {

namespace {

struct Name {
    const char* name;
    uint32_t    bit;
};

// In bit order, so metric_set_names() lists them that way.
constexpr Name kGroupNames[] = {
    { "cpu",        kGroupCpu },
    { "mem",        kGroupMem },
    { "swap",       kGroupSwap },
    { "load",       kGroupLoad },
    { "disk",       kGroupDisk },
    { "net",        kGroupNet },
    { "proc",       kGroupProc },
    { "entropy",    kGroupEntropy },
    { "self",       kGroupSelf },
    { "thermal",    kGroupThermal },
    { "file_watch", kGroupFileWatch },
    { "uptime",     kGroupUptime },
    { "tcp",        kGroupTcp },
    { "udp",        kGroupUdp },
    { "fs",         kGroupFs },
    { "clock",      kGroupClock },
};

// Each collection and the group it comes with.
struct Collection {
    const char* name;
    uint32_t    bit;
    uint32_t    group;
};
constexpr Collection kCollections[] = {
    { "cpu.cores",      kDetailCores,      kGroupCpu },
    { "disk.devices",   kDetailDisks,      kGroupDisk },
    { "net.interfaces", kDetailInterfaces, kGroupNet },
    { "fs.mounts",      kDetailMounts,     kGroupFs },
};

} // namespace

MetricSet metric_set_minimal()  { return MetricSet{kGroupsMinimal, 0}; }
MetricSet metric_set_standard() { return MetricSet{kGroupsSelectable, 0}; }
MetricSet metric_set_full()     { return MetricSet{kGroupsSelectable, kDetailAll}; }

int parse_metric_set(const std::vector<std::string>& names, MetricSet* out,
                     std::string* error) {
    MetricSet r = metric_set_minimal();
    for (const auto& n : names) {
        const char* s = n.c_str();
        if      (n == "minimal")  continue;
        else if (n == "standard") r.groups |= kGroupsSelectable;
        else if (n == "full")     { r.groups |= kGroupsSelectable; r.detail |= kDetailAll; }
        else {
            const uint32_t group = metric_group_bit(s);
            const uint32_t list  = metric_detail_bit(s);
            if (group != 0 && group != kGroupFileWatch) {
                r.groups |= group;
            } else if (list != 0) {
                for (const auto& c : kCollections) {
                    if (c.bit == list) r.groups |= c.group;
                }
                r.detail |= list;
            } else {
                if (error != nullptr) *error = "unknown metric '" + n + "'";
                return -EINVAL;
            }
        }
    }
    if (out != nullptr) *out = r;
    return 0;
}

const char* metric_group_name(uint32_t group_bit) {
    for (const auto& g : kGroupNames) {
        if (g.bit == group_bit) return g.name;
    }
    return nullptr;
}

const char* metric_detail_name(uint32_t detail_bit) {
    for (const auto& c : kCollections) {
        if (c.bit == detail_bit) return c.name;
    }
    return nullptr;
}

uint32_t metric_group_bit(const char* name) {
    for (const auto& g : kGroupNames) {
        if (name != nullptr && std::strcmp(g.name, name) == 0) return g.bit;
    }
    return 0;
}

uint32_t metric_detail_bit(const char* name) {
    for (const auto& c : kCollections) {
        if (name != nullptr && std::strcmp(c.name, name) == 0) return c.bit;
    }
    return 0;
}

std::vector<std::string> metric_set_names(const MetricSet& set) {
    std::vector<std::string> out;
    for (const auto& g : kGroupNames) {
        if ((set.groups & g.bit) != 0) out.emplace_back(g.name);
    }
    for (const auto& c : kCollections) {
        if ((set.detail & c.bit) != 0) out.emplace_back(c.name);
    }
    return out;
}

} // namespace budyk
