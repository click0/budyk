// SPDX-License-Identifier: BSD-3-Clause
#include "core/sample.h"

namespace budyk {

void clear_groups(Sample* s, uint32_t groups) {
    if (s == nullptr) return;
    if ((groups & kGroupCpu)       != 0) s->cpu        = CpuStats{};
    if ((groups & kGroupMem)       != 0) s->mem        = MemStats{};
    if ((groups & kGroupSwap)      != 0) s->swap       = SwapStats{};
    if ((groups & kGroupLoad)      != 0) s->load       = LoadStats{};
    if ((groups & kGroupDisk)      != 0) s->disk       = DiskStats{};
    if ((groups & kGroupNet)       != 0) s->net        = NetStats{};
    if ((groups & kGroupProc)      != 0) s->proc       = ProcessStats{};
    if ((groups & kGroupEntropy)   != 0) s->entropy    = EntropyStats{};
    if ((groups & kGroupSelf)      != 0) s->self_      = SelfStats{};
    if ((groups & kGroupThermal)   != 0) s->thermal    = ThermalStats{};
    if ((groups & kGroupFileWatch) != 0) s->file_watch = FileWatchSample{};
    if ((groups & kGroupUptime)    != 0) s->uptime_seconds = 0.0;
    if ((groups & kGroupTcp)       != 0) s->tcp        = TcpStats{};
    if ((groups & kGroupUdp)       != 0) s->udp        = UdpStats{};
    if ((groups & kGroupIcmp)      != 0) s->icmp       = IcmpStats{};
    if ((groups & kGroupFs)        != 0) s->fs         = FsStats{};
    if ((groups & kGroupClock)     != 0) s->clock      = ClockStats{};
    s->groups &= ~groups;
}

} // namespace budyk
