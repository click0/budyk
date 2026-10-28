// SPDX-License-Identifier: BSD-3-Clause
#include "core/codec.h"
#include "core/endian.h"

#include <cstdint>
#include <cstring>

namespace budyk {

namespace {

constexpr uint8_t  kMagic[8]     = {'B', 'D', 'Y', 'K', 'S', 'M', 'P', 'L'};
constexpr uint32_t kVersionV1    = 1;
constexpr uint32_t kVersionV2    = 2;
constexpr uint32_t kVersionV3    = 3;
constexpr uint32_t kVersionV4    = 4;
constexpr uint32_t kVersionV5    = 5;
constexpr uint32_t kVersionV6    = 6;
constexpr uint32_t kVersionV7    = 7;
constexpr uint32_t kVersionV8    = 8;
constexpr uint32_t kVersionV9    = 9;
constexpr uint32_t kVersionWrite = kVersionV9;

// v1 layout (unchanged — still decoded for records written before the bump).
constexpr size_t kEncodedSizeV1 =
      8 + 4 + 4     // magic, version, flags
    + 8 + 1 + 7     // timestamp_nanos, level, pad
    + 8 + 4 + 4     // cpu.total_percent, cpu.count, pad
    + 8 + 8 + 8     // mem.total, mem.available, mem.available_percent
    + 8 + 8 + 8     // swap.total, swap.used, swap.used_percent
    + 8 + 8 + 8     // load.avg_{1,5,15}m
    + 8;            // uptime_seconds

// v2 appends disk + net aggregates at the end of v1 layout.
constexpr size_t kEncodedSizeV2 =
      kEncodedSizeV1
    + 8 + 8 + 4 + 4 // disk.read_bps, disk.write_bps, disk.device_count, pad
    + 8 + 8 + 4 + 4;// net.rx_bps, net.tx_bps, net.interface_count, pad

// v3 appends proc.{total,running}, padded to 8-byte alignment.
constexpr size_t kEncodedSizeV3 =
      kEncodedSizeV2
    + 4 + 4;        // proc.total, proc.running

// v4 appends entropy.{available_bits, present}, padded to 8 bytes.
constexpr size_t kEncodedSizeV4 =
      kEncodedSizeV3
    + 4 + 1 + 3;    // entropy.available_bits, present, pad

// v5 appends self.{rss, peak_rss, cpu_user_s, cpu_system_s} = 32 bytes.
constexpr size_t kEncodedSizeV5 =
      kEncodedSizeV4
    + 8 + 8 + 8 + 8;

// v6 appends thermal.{max_celsius, sensor_count, present} = 16 bytes
// after padding (8 + 4 + 1 + 3).
constexpr size_t kEncodedSizeV6 =
      kEncodedSizeV5
    + 8 + 4 + 1 + 3;

// v7 appends file_watch.{events_this_tick, watched_count, present} = 16
// bytes (4 + 4 + 1 + 7 pad), keeping the 8-byte-aligned tail convention.
constexpr size_t kEncodedSizeV7 =
      kEncodedSizeV6
    + 4 + 4 + 1 + 7;

// v8 appends the CPU breakdown, the rest of the process states, TCP,
// UDP, the filesystem summary, clock sync and the metric-group mask
// (232 bytes, each block padded to 8).
constexpr size_t kEncodedSizeV8 =
      kEncodedSizeV7
    + 6 * 8                     // cpu: user, system, iowait, irq, steal, idle %
    + 5 * 4 + 4                 // proc: sleeping, blocked, stopped, zombie, threads
    + 4 + 4 + 6 * 8 + 1 + 7     // tcp: established, time_wait, 6 rates, present
    + 4 * 8 + 1 + 7             // udp: 4 rates, present
    + 4 + 1 + 3 + 2 * 8         // fs: count, present, max used %, max inodes %
    + 2 * 8 + 1 + 1 + 6         // clock: offset, max error, synced, present
    + 4 + 4;                    // groups

// v9 appends ICMP: 4 rates, present (40 bytes).
constexpr size_t kEncodedSizeV9 =
      kEncodedSizeV8
    + 4 * 8 + 1 + 7;

constexpr size_t kMinSize[] = {
    0, kEncodedSizeV1, kEncodedSizeV2, kEncodedSizeV3, kEncodedSizeV4,
    kEncodedSizeV5, kEncodedSizeV6, kEncodedSizeV7, kEncodedSizeV8, kEncodedSizeV9,
};

inline void put_u8 (uint8_t*& p, uint8_t  v)         { *p++ = v; }
inline void put_u32(uint8_t*& p, uint32_t v)         { le_put_u32(p, v); p += 4; }
inline void put_u64(uint8_t*& p, uint64_t v)         { le_put_u64(p, v); p += 8; }
inline void put_f64(uint8_t*& p, double   v)         { uint64_t u; std::memcpy(&u, &v, 8); put_u64(p, u); }
inline void put_pad(uint8_t*& p, size_t   n)         { std::memset(p, 0, n); p += n; }

inline uint8_t  get_u8 (const uint8_t*& p)           { return *p++; }
inline uint32_t get_u32(const uint8_t*& p)           { const uint32_t v = le_get_u32(p); p += 4; return v; }
inline uint64_t get_u64(const uint8_t*& p)           { const uint64_t v = le_get_u64(p); p += 8; return v; }
inline double   get_f64(const uint8_t*& p)           { uint64_t u = get_u64(p); double d; std::memcpy(&d, &u, 8); return d; }
inline void     skip   (const uint8_t*& p, size_t n) { p += n; }

} // namespace

size_t sample_max_encoded_size() { return kEncodedSizeV9; }

int sample_encode(const Sample* s, void* buf, size_t cap, size_t* out_len) {
    if (s == nullptr || buf == nullptr || out_len == nullptr) return -1;
    if (cap < kEncodedSizeV9) return -2;

    auto* p = static_cast<uint8_t*>(buf);
    std::memcpy(p, kMagic, 8); p += 8;
    put_u32(p, kVersionWrite);
    put_u32(p, 0);

    put_u64(p, s->timestamp_nanos);
    put_u8 (p, static_cast<uint8_t>(s->level));
    put_pad(p, 7);

    put_f64(p, s->cpu.total_percent);
    put_u32(p, s->cpu.count);
    put_pad(p, 4);

    put_u64(p, s->mem.total);
    put_u64(p, s->mem.available);
    put_f64(p, s->mem.available_percent);

    put_u64(p, s->swap.total);
    put_u64(p, s->swap.used);
    put_f64(p, s->swap.used_percent);

    put_f64(p, s->load.avg_1m);
    put_f64(p, s->load.avg_5m);
    put_f64(p, s->load.avg_15m);

    put_f64(p, s->uptime_seconds);

    // v2 tail: disk + net aggregates.
    put_u64(p, s->disk.read_bytes_per_sec);
    put_u64(p, s->disk.write_bytes_per_sec);
    put_u32(p, s->disk.device_count);
    put_pad(p, 4);

    put_u64(p, s->net.rx_bytes_per_sec);
    put_u64(p, s->net.tx_bytes_per_sec);
    put_u32(p, s->net.interface_count);
    put_pad(p, 4);

    // v3 tail
    put_u32(p, s->proc.total);
    put_u32(p, s->proc.running);

    // v4 tail
    put_u32(p, s->entropy.available_bits);
    put_u8 (p, s->entropy.present ? 1 : 0);
    put_pad(p, 3);

    // v5 tail
    put_u64(p, s->self_.rss_bytes);
    put_u64(p, s->self_.peak_rss_bytes);
    put_f64(p, s->self_.cpu_user_seconds);
    put_f64(p, s->self_.cpu_system_seconds);

    // v6 tail
    put_f64(p, s->thermal.max_celsius);
    put_u32(p, s->thermal.sensor_count);
    put_u8 (p, s->thermal.present ? 1 : 0);
    put_pad(p, 3);

    // v7 tail
    put_u32(p, s->file_watch.events_this_tick);
    put_u32(p, s->file_watch.watched_count);
    put_u8 (p, s->file_watch.present ? 1 : 0);
    put_pad(p, 7);

    // v8
    put_f64(p, s->cpu.user_percent);
    put_f64(p, s->cpu.system_percent);
    put_f64(p, s->cpu.iowait_percent);
    put_f64(p, s->cpu.irq_percent);
    put_f64(p, s->cpu.steal_percent);
    put_f64(p, s->cpu.idle_percent);

    put_u32(p, s->proc.sleeping);
    put_u32(p, s->proc.blocked);
    put_u32(p, s->proc.stopped);
    put_u32(p, s->proc.zombie);
    put_u32(p, s->proc.threads);
    put_pad(p, 4);

    put_u32(p, s->tcp.established);
    put_u32(p, s->tcp.time_wait);
    put_f64(p, s->tcp.active_opens_per_sec);
    put_f64(p, s->tcp.passive_opens_per_sec);
    put_f64(p, s->tcp.retransmits_per_sec);
    put_f64(p, s->tcp.in_segments_per_sec);
    put_f64(p, s->tcp.out_segments_per_sec);
    put_f64(p, s->tcp.in_errors_per_sec);
    put_u8 (p, s->tcp.present ? 1 : 0);
    put_pad(p, 7);

    put_f64(p, s->udp.in_datagrams_per_sec);
    put_f64(p, s->udp.out_datagrams_per_sec);
    put_f64(p, s->udp.no_port_per_sec);
    put_f64(p, s->udp.in_errors_per_sec);
    put_u8 (p, s->udp.present ? 1 : 0);
    put_pad(p, 7);

    put_u32(p, s->fs.count);
    put_u8 (p, s->fs.present ? 1 : 0);
    put_pad(p, 3);
    put_f64(p, s->fs.max_used_percent);
    put_f64(p, s->fs.max_inodes_used_percent);

    put_f64(p, s->clock.offset_ms);
    put_f64(p, s->clock.max_error_ms);
    put_u8 (p, s->clock.synced ? 1 : 0);
    put_u8 (p, s->clock.present ? 1 : 0);
    put_pad(p, 6);

    put_u32(p, s->groups);
    put_pad(p, 4);

    put_f64(p, s->icmp.in_msgs_per_sec);
    put_f64(p, s->icmp.out_msgs_per_sec);
    put_f64(p, s->icmp.in_errors_per_sec);
    put_f64(p, s->icmp.in_dest_unreachs_per_sec);
    put_u8 (p, s->icmp.present ? 1 : 0);
    put_pad(p, 7);

    *out_len = kEncodedSizeV9;
    return 0;
}

int sample_decode(const void* buf, size_t len, Sample* out) {
    if (buf == nullptr || out == nullptr) return -1;
    if (len < kEncodedSizeV1) return -2;

    const auto* p = static_cast<const uint8_t*>(buf);
    if (std::memcmp(p, kMagic, 8) != 0) return -3;
    p += 8;

    uint32_t version = get_u32(p);
    if (version < kVersionV1 || version > kVersionV9) return -4;
    if (len < kMinSize[version])                       return -2;
    (void)get_u32(p);

    out->timestamp_nanos = get_u64(p);
    uint8_t lv = get_u8(p);
    if (lv < 1 || lv > kMaxLevelId) return -5;
    out->level = static_cast<Level>(lv);
    skip(p, 7);

    out->cpu.total_percent = get_f64(p);
    out->cpu.count         = get_u32(p);
    skip(p, 4);

    out->mem.total             = get_u64(p);
    out->mem.available         = get_u64(p);
    out->mem.available_percent = get_f64(p);

    out->swap.total        = get_u64(p);
    out->swap.used         = get_u64(p);
    out->swap.used_percent = get_f64(p);

    out->load.avg_1m  = get_f64(p);
    out->load.avg_5m  = get_f64(p);
    out->load.avg_15m = get_f64(p);

    out->uptime_seconds = get_f64(p);

    if (version >= kVersionV2) {
        out->disk.read_bytes_per_sec  = get_u64(p);
        out->disk.write_bytes_per_sec = get_u64(p);
        out->disk.device_count        = get_u32(p);
        skip(p, 4);

        out->net.rx_bytes_per_sec     = get_u64(p);
        out->net.tx_bytes_per_sec     = get_u64(p);
        out->net.interface_count      = get_u32(p);
        skip(p, 4);
    } else {
        // v1 records carry no disk/net data — leave fields zeroed.
        out->disk = DiskStats{};
        out->net  = NetStats{};
    }

    if (version >= kVersionV3) {
        out->proc.total   = get_u32(p);
        out->proc.running = get_u32(p);
    } else {
        // v1 / v2 records carry no proc data — leave fields zeroed.
        out->proc = ProcessStats{};
    }

    if (version >= kVersionV4) {
        out->entropy.available_bits = get_u32(p);
        out->entropy.present        = (get_u8(p) != 0);
        skip(p, 3);
    } else {
        // v1 / v2 / v3 records carry no entropy data.
        out->entropy = EntropyStats{};
    }

    if (version >= kVersionV5) {
        out->self_.rss_bytes          = get_u64(p);
        out->self_.peak_rss_bytes     = get_u64(p);
        out->self_.cpu_user_seconds   = get_f64(p);
        out->self_.cpu_system_seconds = get_f64(p);
    } else {
        // v1..v4 records carry no self data.
        out->self_ = SelfStats{};
    }

    if (version >= kVersionV6) {
        out->thermal.max_celsius  = get_f64(p);
        out->thermal.sensor_count = get_u32(p);
        out->thermal.present      = (get_u8(p) != 0);
        skip(p, 3);
    } else {
        // v1..v5 records carry no thermal data.
        out->thermal = ThermalStats{};
    }

    if (version >= kVersionV7) {
        out->file_watch.events_this_tick = get_u32(p);
        out->file_watch.watched_count    = get_u32(p);
        out->file_watch.present          = (get_u8(p) != 0);
        skip(p, 7);
    } else {
        // v1..v6 records carry no file-watch data.
        out->file_watch = FileWatchSample{};
    }

    out->tcp   = TcpStats{};
    out->udp   = UdpStats{};
    out->fs    = FsStats{};
    out->clock = ClockStats{};
    out->proc.sleeping = out->proc.blocked = out->proc.stopped = 0;
    out->proc.zombie   = out->proc.threads = 0;
    out->cpu.user_percent = out->cpu.system_percent = out->cpu.iowait_percent = 0.0;
    out->cpu.irq_percent  = out->cpu.steal_percent  = out->cpu.idle_percent   = 0.0;
    out->icmp = IcmpStats{};
    if (version < kVersionV8) {
        // Before v8 every group a record held was always collected, and
        // what it holds is its version's: v1 cpu, mem, swap, load and
        // uptime; v2 adds disk and net, v3 proc, v4 entropy, v5 self, v6
        // thermal, v7 file_watch. Every one up to file_watch was claimed,
        // and a v2 record's zeros (proc.total, self.rss) read as readings,
        // in the rings it was converted to too.
        uint32_t g = kGroupCpu | kGroupMem | kGroupSwap | kGroupLoad | kGroupUptime;
        if (version >= kVersionV2) g |= kGroupDisk | kGroupNet;
        if (version >= kVersionV3) g |= kGroupProc;
        if (version >= kVersionV4) g |= kGroupEntropy;
        if (version >= kVersionV5) g |= kGroupSelf;
        if (version >= kVersionV6) g |= kGroupThermal;
        if (version >= kVersionV7) g |= kGroupFileWatch;
        out->groups = g;
        return 0;
    }
    out->cpu.user_percent   = get_f64(p);
    out->cpu.system_percent = get_f64(p);
    out->cpu.iowait_percent = get_f64(p);
    out->cpu.irq_percent    = get_f64(p);
    out->cpu.steal_percent  = get_f64(p);
    out->cpu.idle_percent   = get_f64(p);

    out->proc.sleeping = get_u32(p);
    out->proc.blocked  = get_u32(p);
    out->proc.stopped  = get_u32(p);
    out->proc.zombie   = get_u32(p);
    out->proc.threads  = get_u32(p);
    skip(p, 4);

    out->tcp.established           = get_u32(p);
    out->tcp.time_wait             = get_u32(p);
    out->tcp.active_opens_per_sec  = get_f64(p);
    out->tcp.passive_opens_per_sec = get_f64(p);
    out->tcp.retransmits_per_sec   = get_f64(p);
    out->tcp.in_segments_per_sec   = get_f64(p);
    out->tcp.out_segments_per_sec  = get_f64(p);
    out->tcp.in_errors_per_sec     = get_f64(p);
    out->tcp.present               = (get_u8(p) != 0);
    skip(p, 7);

    out->udp.in_datagrams_per_sec  = get_f64(p);
    out->udp.out_datagrams_per_sec = get_f64(p);
    out->udp.no_port_per_sec       = get_f64(p);
    out->udp.in_errors_per_sec     = get_f64(p);
    out->udp.present               = (get_u8(p) != 0);
    skip(p, 7);

    out->fs.count   = get_u32(p);
    out->fs.present = (get_u8(p) != 0);
    skip(p, 3);
    out->fs.max_used_percent        = get_f64(p);
    out->fs.max_inodes_used_percent = get_f64(p);

    out->clock.offset_ms    = get_f64(p);
    out->clock.max_error_ms = get_f64(p);
    out->clock.synced       = (get_u8(p) != 0);
    out->clock.present      = (get_u8(p) != 0);
    skip(p, 6);

    out->groups = get_u32(p);
    skip(p, 4);

    if (version < kVersionV9) return 0;    // no ICMP before v9; groups says so
    out->icmp.in_msgs_per_sec          = get_f64(p);
    out->icmp.out_msgs_per_sec         = get_f64(p);
    out->icmp.in_errors_per_sec        = get_f64(p);
    out->icmp.in_dest_unreachs_per_sec = get_f64(p);
    out->icmp.present                  = (get_u8(p) != 0);
    skip(p, 7);
    return 0;
}

} // namespace budyk
