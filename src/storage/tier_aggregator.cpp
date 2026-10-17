// SPDX-License-Identifier: BSD-3-Clause
#include "storage/tier_aggregator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace budyk {

namespace {

uint64_t mean_u64(double sum, double inv_n) {
    return static_cast<uint64_t>(std::llround(sum * inv_n));
}

} // namespace

TierAggregator::TierAggregator(Level out_level)
    : out_level_(out_level) {
    reset();
}

void TierAggregator::reset() {
    n_                          = 0;
    first_ts_                   = 0;
    for (uint32_t& v : group_n_) v = 0;
    cpu_total_percent_sum_      = 0.0;
    mem_available_sum_          = 0.0;
    mem_available_percent_sum_  = 0.0;
    swap_used_sum_              = 0.0;
    swap_used_percent_sum_      = 0.0;
    load_1m_sum_                = 0.0;
    load_5m_sum_                = 0.0;
    load_15m_sum_               = 0.0;
    disk_read_sum_              = 0.0;
    disk_write_sum_             = 0.0;
    net_rx_sum_                 = 0.0;
    net_tx_sum_                 = 0.0;
    proc_total_max_             = 0;
    proc_running_max_           = 0;
    thermal_max_                = 0.0;
    rss_max_                    = 0;
    entropy_min_                = UINT32_MAX;
    file_events_sum_            = 0;
    entropy_present_            = false;
    thermal_present_            = false;
    file_watch_present_         = false;
    for (double& v : cpu_part_sum_) v = 0.0;
    for (double& v : tcp_rate_sum_) v = 0.0;
    for (double& v : udp_rate_sum_) v = 0.0;
    proc_max_    = ProcessStats{};
    tcp_         = TcpStats{};
    udp_present_ = false;
    fs_max_      = FsStats{};
    clock_       = ClockStats{};
    clock_.synced = true;          // until a sample that read the clock says not
    last_ = Sample{};
}

void TierAggregator::add(const Sample& s) {
    if (n_ == 0) first_ts_ = s.timestamp_nanos;
    ++n_;
    last_.timestamp_nanos = s.timestamp_nanos;

    // Each group from the samples that have it; its "last" fields from
    // the last of them.
    const uint32_t g = s.groups;
    for (unsigned i = 0; i < kGroupCount; ++i) {
        if ((g & (1u << i)) != 0) ++group_n_[i];
    }
    if ((g & kGroupCpu) != 0) {
        cpu_total_percent_sum_ += s.cpu.total_percent;
        const double parts[6] = { s.cpu.user_percent, s.cpu.system_percent, s.cpu.iowait_percent,
                                  s.cpu.irq_percent, s.cpu.steal_percent, s.cpu.idle_percent };
        for (int i = 0; i < 6; ++i) cpu_part_sum_[i] += parts[i];
        last_.cpu = s.cpu;
    }
    if ((g & kGroupMem) != 0) {
        mem_available_sum_         += static_cast<double>(s.mem.available);
        mem_available_percent_sum_ += s.mem.available_percent;
        last_.mem = s.mem;
    }
    if ((g & kGroupSwap) != 0) {
        swap_used_sum_         += static_cast<double>(s.swap.used);
        swap_used_percent_sum_ += s.swap.used_percent;
        last_.swap = s.swap;
    }
    if ((g & kGroupLoad) != 0) {
        load_1m_sum_  += s.load.avg_1m;
        load_5m_sum_  += s.load.avg_5m;
        load_15m_sum_ += s.load.avg_15m;
    }
    if ((g & kGroupDisk) != 0) {
        disk_read_sum_  += static_cast<double>(s.disk.read_bytes_per_sec);
        disk_write_sum_ += static_cast<double>(s.disk.write_bytes_per_sec);
        last_.disk = s.disk;
    }
    if ((g & kGroupNet) != 0) {
        net_rx_sum_ += static_cast<double>(s.net.rx_bytes_per_sec);
        net_tx_sum_ += static_cast<double>(s.net.tx_bytes_per_sec);
        last_.net = s.net;
    }
    if ((g & kGroupProc) != 0) {
        proc_total_max_    = std::max(proc_total_max_,    s.proc.total);
        proc_running_max_  = std::max(proc_running_max_,  s.proc.running);
        proc_max_.sleeping = std::max(proc_max_.sleeping, s.proc.sleeping);
        proc_max_.blocked  = std::max(proc_max_.blocked,  s.proc.blocked);
        proc_max_.stopped  = std::max(proc_max_.stopped,  s.proc.stopped);
        proc_max_.zombie   = std::max(proc_max_.zombie,   s.proc.zombie);
        proc_max_.threads  = std::max(proc_max_.threads,  s.proc.threads);
    }
    if ((g & kGroupSelf) != 0) {
        rss_max_    = std::max(rss_max_, s.self_.rss_bytes);
        last_.self_ = s.self_;
    }
    if ((g & kGroupThermal) != 0) {
        if (s.thermal.present) {
            thermal_max_     = thermal_present_ ? std::max(thermal_max_, s.thermal.max_celsius)
                                                : s.thermal.max_celsius;
            thermal_present_ = true;
        }
        last_.thermal = s.thermal;
    }
    if ((g & kGroupEntropy) != 0 && s.entropy.present) {
        entropy_min_     = std::min(entropy_min_, s.entropy.available_bits);
        entropy_present_ = true;
    }
    if ((g & kGroupFileWatch) != 0) {
        file_events_sum_    += s.file_watch.events_this_tick;
        file_watch_present_  = file_watch_present_ || s.file_watch.present;
        last_.file_watch     = s.file_watch;
    }
    if ((g & kGroupUptime) != 0) last_.uptime_seconds = s.uptime_seconds;
    if ((g & kGroupTcp) != 0) {
        const double rates[6] = { s.tcp.active_opens_per_sec, s.tcp.passive_opens_per_sec,
                                  s.tcp.retransmits_per_sec, s.tcp.in_segments_per_sec,
                                  s.tcp.out_segments_per_sec, s.tcp.in_errors_per_sec };
        for (int i = 0; i < 6; ++i) tcp_rate_sum_[i] += rates[i];
        tcp_.established = std::max(tcp_.established, s.tcp.established);
        tcp_.time_wait   = std::max(tcp_.time_wait,   s.tcp.time_wait);
        tcp_.present     = tcp_.present || s.tcp.present;
    }
    if ((g & kGroupUdp) != 0) {
        const double rates[4] = { s.udp.in_datagrams_per_sec, s.udp.out_datagrams_per_sec,
                                  s.udp.no_port_per_sec, s.udp.in_errors_per_sec };
        for (int i = 0; i < 4; ++i) udp_rate_sum_[i] += rates[i];
        udp_present_ = udp_present_ || s.udp.present;
    }
    if ((g & kGroupFs) != 0) {
        fs_max_.max_used_percent        = std::max(fs_max_.max_used_percent, s.fs.max_used_percent);
        fs_max_.max_inodes_used_percent = std::max(fs_max_.max_inodes_used_percent,
                                                   s.fs.max_inodes_used_percent);
        fs_max_.present = fs_max_.present || s.fs.present;
        last_.fs        = s.fs;
    }
    if ((g & kGroupClock) != 0 && s.clock.present) {
        if (!clock_.present || std::fabs(s.clock.offset_ms) > std::fabs(clock_.offset_ms)) {
            clock_.offset_ms = s.clock.offset_ms;
        }
        clock_.max_error_ms = std::max(clock_.max_error_ms, s.clock.max_error_ms);
        clock_.synced       = clock_.synced && s.clock.synced;
        clock_.present      = true;
    }
}

size_t TierAggregator::size() const { return n_; }

uint64_t TierAggregator::first_timestamp() const { return first_ts_; }

bool TierAggregator::fold(Sample* out) {
    if (out == nullptr) { reset(); return false; }
    if (n_ == 0)        { reset(); return false; }

    // 1 / (samples that had group i), for its means.
    double inv[kGroupCount];
    uint32_t groups = 0;
    for (unsigned i = 0; i < kGroupCount; ++i) {
        inv[i] = group_n_[i] > 0 ? 1.0 / static_cast<double>(group_n_[i]) : 0.0;
        if (group_n_[i] > 0) groups |= 1u << i;
    }
    auto inv_n = [&](uint32_t bit) {
        unsigned i = 0;
        while ((bit >> i) != 1u) ++i;
        return inv[i];
    };

    // Start from the last values: every "last" field is already right.
    Sample r = last_;
    r.level  = out_level_;
    r.folded = true;

    r.cpu.total_percent     = cpu_total_percent_sum_ * inv_n(kGroupCpu);
    r.mem.available         = mean_u64(mem_available_sum_, inv_n(kGroupMem));
    r.mem.available_percent = mem_available_percent_sum_ * inv_n(kGroupMem);
    r.swap.used             = mean_u64(swap_used_sum_, inv_n(kGroupSwap));
    r.swap.used_percent     = swap_used_percent_sum_ * inv_n(kGroupSwap);
    r.load.avg_1m           = load_1m_sum_  * inv_n(kGroupLoad);
    r.load.avg_5m           = load_5m_sum_  * inv_n(kGroupLoad);
    r.load.avg_15m          = load_15m_sum_ * inv_n(kGroupLoad);
    r.disk.read_bytes_per_sec  = mean_u64(disk_read_sum_,  inv_n(kGroupDisk));
    r.disk.write_bytes_per_sec = mean_u64(disk_write_sum_, inv_n(kGroupDisk));
    r.net.rx_bytes_per_sec     = mean_u64(net_rx_sum_, inv_n(kGroupNet));
    r.net.tx_bytes_per_sec     = mean_u64(net_tx_sum_, inv_n(kGroupNet));

    r.proc.total          = proc_total_max_;
    r.proc.running        = proc_running_max_;
    r.self_.rss_bytes     = rss_max_;
    r.thermal.present     = thermal_present_;
    r.thermal.max_celsius = thermal_present_ ? thermal_max_ : 0.0;
    r.entropy.present        = entropy_present_;
    r.entropy.available_bits = entropy_present_ ? entropy_min_ : 0;
    r.file_watch.present          = file_watch_present_;
    r.file_watch.events_this_tick = static_cast<uint32_t>(
        std::min<uint64_t>(file_events_sum_, UINT32_MAX));

    r.cpu.user_percent   = cpu_part_sum_[0] * inv_n(kGroupCpu);
    r.cpu.system_percent = cpu_part_sum_[1] * inv_n(kGroupCpu);
    r.cpu.iowait_percent = cpu_part_sum_[2] * inv_n(kGroupCpu);
    r.cpu.irq_percent    = cpu_part_sum_[3] * inv_n(kGroupCpu);
    r.cpu.steal_percent  = cpu_part_sum_[4] * inv_n(kGroupCpu);
    r.cpu.idle_percent   = cpu_part_sum_[5] * inv_n(kGroupCpu);
    r.proc.sleeping = proc_max_.sleeping;
    r.proc.blocked  = proc_max_.blocked;
    r.proc.stopped  = proc_max_.stopped;
    r.proc.zombie   = proc_max_.zombie;
    r.proc.threads  = proc_max_.threads;
    r.tcp.established           = tcp_.established;
    r.tcp.time_wait             = tcp_.time_wait;
    r.tcp.active_opens_per_sec  = tcp_rate_sum_[0] * inv_n(kGroupTcp);
    r.tcp.passive_opens_per_sec = tcp_rate_sum_[1] * inv_n(kGroupTcp);
    r.tcp.retransmits_per_sec   = tcp_rate_sum_[2] * inv_n(kGroupTcp);
    r.tcp.in_segments_per_sec   = tcp_rate_sum_[3] * inv_n(kGroupTcp);
    r.tcp.out_segments_per_sec  = tcp_rate_sum_[4] * inv_n(kGroupTcp);
    r.tcp.in_errors_per_sec     = tcp_rate_sum_[5] * inv_n(kGroupTcp);
    r.tcp.present               = tcp_.present;
    r.udp.in_datagrams_per_sec  = udp_rate_sum_[0] * inv_n(kGroupUdp);
    r.udp.out_datagrams_per_sec = udp_rate_sum_[1] * inv_n(kGroupUdp);
    r.udp.no_port_per_sec       = udp_rate_sum_[2] * inv_n(kGroupUdp);
    r.udp.in_errors_per_sec     = udp_rate_sum_[3] * inv_n(kGroupUdp);
    r.udp.present               = udp_present_;
    r.fs.max_used_percent        = fs_max_.max_used_percent;
    r.fs.max_inodes_used_percent = fs_max_.max_inodes_used_percent;
    r.fs.present                 = fs_max_.present;
    r.clock  = clock_;
    if (!clock_.present) r.clock.synced = false;
    r.groups = groups;

    *out = r;
    reset();
    return true;
}

} // namespace budyk
