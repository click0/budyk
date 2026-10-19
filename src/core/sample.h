// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>

namespace budyk {

enum class Level : uint8_t { L1 = 1, L2 = 2, L3 = 3 };

/// Built-in levels are 1..3. User-defined levels (collection.levels in the
/// config) get ids kFirstCustomLevel, kFirstCustomLevel + 1, ... in config
/// order. The id is what a stored record's level byte holds.
constexpr uint8_t kFirstCustomLevel = 4;
constexpr uint8_t kMaxCustomLevels  = 16;
constexpr uint8_t kMaxLevelId       = kFirstCustomLevel + kMaxCustomLevels - 1;

/// total_percent is everything but idle (and iowait). The parts add up to
/// 100: user includes nice, irq softirq; iowait and steal stay 0 on
/// FreeBSD, which has neither.
struct CpuStats {
    double   total_percent;
    uint32_t count;
    double   user_percent;
    double   system_percent;
    double   iowait_percent;
    double   irq_percent;
    double   steal_percent;
    double   idle_percent;
};
struct MemStats   { uint64_t total; uint64_t available; double available_percent; };
struct SwapStats  { uint64_t total; uint64_t used; double used_percent; };
struct LoadStats  { double avg_1m, avg_5m, avg_15m; };

/// Aggregate disk I/O across all whole block devices (partitions excluded).
struct DiskStats {
    uint64_t read_bytes_per_sec;
    uint64_t write_bytes_per_sec;
    uint32_t device_count;
};

/// Aggregate network I/O across all non-loopback interfaces.
struct NetStats {
    uint64_t rx_bytes_per_sec;
    uint64_t tx_bytes_per_sec;
    uint32_t interface_count;
};

/// Processes by state — L2 / watchful tier (spec §3.3.2, §2.1). total
/// counts processes (not threads); blocked is an uninterruptible wait
/// (Linux D, FreeBSD SLOCK); threads are summed over every process.
struct ProcessStats {
    uint32_t total;
    uint32_t running;
    uint32_t sleeping;
    uint32_t blocked;
    uint32_t stopped;
    uint32_t zombie;
    uint32_t threads;
};

/// TCP and UDP over all address families (spec §2.1): sockets in two
/// states that matter, and per-second rates of the cumulative counters.
/// present is false when the kernel counters could not be read.
struct TcpStats {
    uint32_t established;
    uint32_t time_wait;
    double   active_opens_per_sec;
    double   passive_opens_per_sec;
    double   retransmits_per_sec;
    double   in_segments_per_sec;
    double   out_segments_per_sec;
    double   in_errors_per_sec;
    bool     present;
};
struct UdpStats {
    double   in_datagrams_per_sec;
    double   out_datagrams_per_sec;
    double   no_port_per_sec;
    double   in_errors_per_sec;
    bool     present;
};

/// ICMP over IPv4 (the kernel's Icmp MIB; ICMPv6 is another protocol and
/// not counted here): messages in and out, errors in (bad checksum, too
/// short, bad code or length), destination unreachable received, per
/// second.
struct IcmpStats {
    double   in_msgs_per_sec;
    double   out_msgs_per_sec;
    double   in_errors_per_sec;
    double   in_dest_unreachs_per_sec;
    bool     present;
};

/// Local filesystems (spec §2.1; pseudo, read-only and network ones are
/// skipped): how many, and the fullest by space and by inodes.
struct FsStats {
    uint32_t count;
    double   max_used_percent;
    double   max_inodes_used_percent;
    bool     present;
};

/// Clock sync (spec §3.4.1 L3, §2.1): the kernel's NTP discipline state
/// from ntp_adjtime(2).
struct ClockStats {
    double   offset_ms;
    double   max_error_ms;
    bool     synced;
    bool     present;
};

/// Which metric groups a sample carries (Sample::groups). Every group is
/// collected today; a record from before codec v8 has the v7 groups.
enum MetricGroup : uint32_t {
    kGroupCpu       = 1u << 0,
    kGroupMem       = 1u << 1,
    kGroupSwap      = 1u << 2,
    kGroupLoad      = 1u << 3,
    kGroupDisk      = 1u << 4,
    kGroupNet       = 1u << 5,
    kGroupProc      = 1u << 6,
    kGroupEntropy   = 1u << 7,
    kGroupSelf      = 1u << 8,
    kGroupThermal   = 1u << 9,
    kGroupFileWatch = 1u << 10,
    kGroupUptime    = 1u << 11,
    kGroupTcp       = 1u << 12,
    kGroupUdp       = 1u << 13,
    kGroupFs        = 1u << 14,
    kGroupClock     = 1u << 15,
    kGroupIcmp      = 1u << 16,
};
constexpr uint32_t kGroupsV7  = (1u << 12) - 1;    ///< cpu .. uptime
constexpr uint32_t kGroupsAll = (1u << 17) - 1;

/// Entropy pool state — L3 / active tier (spec §3.3.3).
/// On platforms without an entropy_avail surface (currently FreeBSD),
/// available_bits stays at 0 and `present` is false.
struct EntropyStats {
    uint32_t available_bits;
    bool     present;
};

/// Self-metrics — the daemon's own RSS / VSZ / CPU consumption
/// (spec §3.3.3). All fields are best-effort; unavailable data
/// is reported as 0 rather than a separate present flag.
struct SelfStats {
    uint64_t rss_bytes;          ///< current resident set; Linux only
    uint64_t peak_rss_bytes;     ///< peak ever, from getrusage(2)
    double   cpu_user_seconds;
    double   cpu_system_seconds;
};

/// Thermal sensors — L3 / active tier (spec §3.3.3).
/// Reports the hottest reading across every sensor we can probe.
///   * Linux:   /sys/class/thermal/thermal_zone*/temp (millidegrees C)
///   * FreeBSD: `dev.cpu.<N>.temperature` sysctl (Kelvin × 10)
/// `present == false` when no sensors are exposed (containers, VMs
/// without ACPI, jails). `max_celsius` and `sensor_count` stay 0 in
/// that case.
struct ThermalStats {
    double   max_celsius;
    uint32_t sensor_count;
    bool     present;
};

/// File-watch summary — populated only when security.file_watch is
/// enabled (otherwise present=false and the SPA hides the row). Reports
/// per-tick activity so the dashboard shows a spiky timeline of *when*
/// watched files changed, rather than a continuous gauge.
struct FileWatchSample {
    uint32_t events_this_tick;   ///< distinct watched paths that fired an
                                 ///< event since the previous tick
    uint32_t watched_count;      ///< configured paths under watch
    bool     present;            ///< file_watch enabled this run
};

struct Sample {
    uint64_t timestamp_nanos;
    Level    level;
    CpuStats  cpu;
    MemStats  mem;
    SwapStats swap;
    LoadStats load;
    DiskStats disk;
    NetStats  net;
    ProcessStats proc;
    EntropyStats entropy;
    SelfStats    self_;
    ThermalStats thermal;
    FileWatchSample file_watch;
    TcpStats   tcp;
    UdpStats   udp;
    IcmpStats  icmp;
    FsStats    fs;
    ClockStats clock;
    uint32_t   groups;        ///< MetricGroup bits
    /// Groups this tick read for the first time in a row: a rate's
    /// baseline, so not in `groups` yet. This tick's only: not stored.
    uint32_t   baseline;
    double    uptime_seconds;
    /// True for a record folded from finer samples by TierAggregator (an
    /// L2 or L1 summary of an L3 or custom-level stretch). Not part of
    /// the sample payload: the ring record carries it in its header
    /// (storage/codec.h), like the timestamp and the level.
    bool      folded;
};

/// Zeroes the groups in `groups` (MetricGroup bits) and drops them from
/// s->groups: what a sample says about a group it did not collect.
void clear_groups(Sample* s, uint32_t groups);

} // namespace budyk
