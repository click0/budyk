// SPDX-License-Identifier: BSD-3-Clause
#ifndef BUDYK_SAMPLE_C_H
#define BUDYK_SAMPLE_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// C-compatible view of budyk::Sample. Lets plain-C collectors
/// (`src/collector/<platform>/*.c`) populate samples without pulling in
/// C++ headers. The C++ side is responsible for converting between
/// budyk_sample_c and budyk::Sample when wiring the MetricSource.
typedef struct {
    double   total_percent;
    uint32_t count;
    /** Where the time went, in percent of all CPU time since the last
     * tick (they add up to 100). user includes nice, irq softirq;
     * iowait and steal are 0 on FreeBSD, which has neither. */
    double   user_percent;
    double   system_percent;
    double   iowait_percent;
    double   irq_percent;
    double   steal_percent;
    double   idle_percent;
} budyk_cpu_c;

typedef struct {
    uint64_t total;
    uint64_t available;
    double   available_percent;
} budyk_mem_c;

typedef struct {
    uint64_t total;
    uint64_t used;
    double   used_percent;
} budyk_swap_c;

typedef struct {
    double avg_1m;
    double avg_5m;
    double avg_15m;
} budyk_load_c;

typedef struct {
    uint64_t read_bytes_per_sec;
    uint64_t write_bytes_per_sec;
    uint32_t device_count;
} budyk_disk_c;

typedef struct {
    uint64_t rx_bytes_per_sec;
    uint64_t tx_bytes_per_sec;
    uint32_t interface_count;
} budyk_net_c;

typedef struct {
    uint32_t total;      /**< processes */
    uint32_t running;
    uint32_t sleeping;
    uint32_t blocked;    /**< uninterruptible wait (Linux D, FreeBSD SLOCK) */
    uint32_t stopped;
    uint32_t zombie;
    uint32_t threads;    /**< across all processes */
} budyk_proc_c;

/** TCP and UDP, all address families. Rates are per second since the
 * last tick; established and time_wait are the sockets in that state
 * now. present = 0 when the kernel counters could not be read. */
typedef struct {
    uint32_t established;
    uint32_t time_wait;
    double   active_opens_per_sec;
    double   passive_opens_per_sec;
    double   retransmits_per_sec;
    double   in_segments_per_sec;
    double   out_segments_per_sec;
    double   in_errors_per_sec;
    uint8_t  present;
} budyk_tcp_c;
typedef struct {
    double   in_datagrams_per_sec;
    double   out_datagrams_per_sec;
    double   no_port_per_sec;
    double   in_errors_per_sec;
    uint8_t  present;
} budyk_udp_c;
typedef struct {
    double   in_msgs_per_sec;
    double   out_msgs_per_sec;
    double   in_errors_per_sec;
    double   in_dest_unreachs_per_sec;
    uint8_t  present;
} budyk_icmp_c;

/** Local filesystems (pseudo, read-only and network ones are skipped):
 * how many, and the fullest by space and by inodes. */
typedef struct {
    uint32_t count;
    double   max_used_percent;
    double   max_inodes_used_percent;
    uint8_t  present;
} budyk_fs_c;

/** The kernel's clock discipline (ntp_adjtime(2)): whether it considers
 * the clock synchronised, its offset and maximum error. */
typedef struct {
    double   offset_ms;
    double   max_error_ms;
    uint8_t  synced;
    uint8_t  present;
} budyk_clock_c;

typedef struct {
    uint32_t available_bits;
    uint8_t  present;       /**< 0 = platform has no entropy_avail surface */
} budyk_entropy_c;

typedef struct {
    uint64_t rss_bytes;
    uint64_t peak_rss_bytes;
    double   cpu_user_seconds;
    double   cpu_system_seconds;
} budyk_self_c;

typedef struct {
    double   max_celsius;
    uint32_t sensor_count;
    uint8_t  present;       /**< 0 = no sensors exposed on this host */
} budyk_thermal_c;

typedef struct {
    uint64_t        timestamp_nanos;
    uint8_t         level;
    budyk_cpu_c     cpu;
    budyk_mem_c     mem;
    budyk_swap_c    swap;
    budyk_load_c    load;
    budyk_disk_c    disk;
    budyk_net_c     net;
    budyk_proc_c    proc;
    budyk_entropy_c entropy;
    budyk_self_c    self_;
    budyk_thermal_c thermal;
    budyk_tcp_c     tcp;
    budyk_udp_c     udp;
    budyk_icmp_c    icmp;
    budyk_fs_c      fs;
    budyk_clock_c   clock;
    double          uptime_seconds;
} budyk_sample_c;

/// Linux collectors. Return 0 on success, negative errno on failure.
int budyk_collect_memory_linux(budyk_sample_c* s);
int budyk_collect_load_linux  (budyk_sample_c* s);
int budyk_collect_uptime_linux(budyk_sample_c* s);

/// CPU collection requires state across ticks — the ctx carries the
/// previous /proc/stat snapshot for delta computation. Initialise the
/// struct to all zeros before the first call. The first call records
/// a baseline and sets cpu.total_percent to 0.
typedef struct {
    uint64_t busy;      ///< user + nice + system + irq + softirq + steal
    uint64_t total;     ///< busy + idle + iowait
    /// The previous snapshot of each part: user (+ nice), system, iowait,
    /// irq (+ softirq), steal, idle.
    uint64_t part[6];
    int      has_prev;
} budyk_cpu_ctx_c;

/// TCP / UDP counters are cumulative; the rates come from the delta
/// since the previous call, like disk and network bytes.
typedef struct {
    uint64_t tcp_active_opens, tcp_passive_opens, tcp_retransmits;
    uint64_t tcp_in_segments, tcp_out_segments, tcp_in_errors;
    uint64_t udp_in, udp_out, udp_no_port, udp_in_errors;
    uint64_t icmp_in, icmp_out, icmp_in_errors, icmp_in_unreach;
    uint64_t prev_ns;
    int      has_prev;
} budyk_netstat_ctx_c;

/// Per-entity detail (spec §2.1): each core, disk, interface and local
/// filesystem, and the top processes. Not stored in the rings; the daemon serves the latest to
/// the dashboard and the rules. One instance lives across ticks: besides
/// the lists it keeps each entity's previous counters for the rates, so
/// zero it once and pass the same one every time.
#define BUDYK_MAX_CORES    1024
#define BUDYK_MAX_ENTITIES 256    /**< disks, interfaces, filesystems each listed */
#define BUDYK_TOP_EACH     10     /**< the top processes by CPU, and by RSS */
#define BUDYK_MAX_TRACKED_PROCS 8192   /**< processes whose CPU time is kept for a rate */

typedef struct {
    double total_percent;
    double user_percent, system_percent, iowait_percent;
    double irq_percent, steal_percent, idle_percent;
    uint32_t id;   /**< the CPU's number (cpuN): with one offline the list skips it */
} budyk_core_c;

typedef struct {
    char     name[32];             /**< sda, nvme0n1, ada0 */
    uint64_t read_bytes_per_sec;
    uint64_t write_bytes_per_sec;
    double   reads_per_sec;
    double   writes_per_sec;
    double   busy_percent;         /**< time the device had I/O in flight */
} budyk_disk_dev_c;

typedef struct {
    char     name[32];             /**< eth0, em0 */
    uint64_t rx_bytes_per_sec;
    uint64_t tx_bytes_per_sec;
    double   rx_packets_per_sec;
    double   tx_packets_per_sec;
    double   rx_errors_per_sec;
    double   tx_errors_per_sec;
    double   rx_drops_per_sec;
    uint8_t  up;
} budyk_iface_c;

typedef struct {
    char     mount[256];
    char     type[32];
    char     device[128];
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t avail_bytes;          /**< to unprivileged users, as df */
    double   used_percent;
    uint64_t inodes_total;
    uint64_t inodes_used;
    double   inodes_used_percent;
} budyk_mount_c;

/** One of the top processes. Kernel threads are left out. */
typedef struct {
    int32_t  pid;
    char     name[32];             /**< comm, as the kernel has it */
    char     state;                /**< as ps(1) shows it: R S D T Z ... */
    uint32_t threads;
    double   cpu_percent;          /**< of one CPU, over the last interval */
    uint64_t rss_bytes;
} budyk_process_c;

/** One process as a collector read it: what the tracker needs. */
typedef struct {
    int32_t  pid;
    uint64_t start;                /**< start time (any unit): tells a reused pid */
    uint64_t cpu;                  /**< cumulative CPU time, in cpu_per_sec units */
    uint64_t rss_bytes;
    uint32_t threads;
    char     state;
    char     name[32];
} budyk_proc_read_c;

/** A process's CPU time at the previous tick, by pid. */
typedef struct {
    int32_t  pid;
    uint64_t start;
    uint64_t cpu;
} budyk_proc_prev_c;

/** An entity's cumulative counters at the previous tick, by name. */
typedef struct {
    char     name[32];
    uint64_t c[8];
} budyk_entity_prev_c;

#define BUDYK_MAX_COUNTED 1024  /**< disks or interfaces the total rates follow, each */

/** A device's two byte counters at the previous call, by name: what the
 *  total disk and network rates sum the deltas of. */
typedef struct {
    char     name[32];
    uint64_t a, b;
} budyk_counted_c;

typedef struct {
    /** The lists this tick has (MetricDetail bits, core/metric_set.h),
     * and those it read for the first time in a row (a rate's baseline,
     * not in `collected` yet); set by the daemon's collect_one(), not by
     * the collectors. */
    uint32_t         collected;
    uint32_t         baseline;
    /** How many of each list this tick has; the 32-bit fields go
     * together, so the struct is not padded between them. */
    uint32_t         ncores, ndisks, nifaces, nmounts, nprocs;
    /** For the rates: entries in disk_prev / iface_prev, in each
     * proc_track, and in the candidates. */
    uint32_t         ndisk_prev, niface_prev;
    uint32_t         nproc_track[2];
    uint32_t         ntop_cpu, ntop_rss;

    budyk_core_c     cores[BUDYK_MAX_CORES];
    budyk_disk_dev_c disks[BUDYK_MAX_ENTITIES];
    budyk_iface_c    ifaces[BUDYK_MAX_ENTITIES];
    budyk_mount_c    mounts[BUDYK_MAX_ENTITIES];
    /** The union of the top BUDYK_TOP_EACH by CPU and by RSS, by CPU
     * then RSS, largest first. */
    budyk_process_c  procs[2 * BUDYK_TOP_EACH];

    /** Across ticks, for the rates. */
    uint64_t            core_prev[BUDYK_MAX_CORES][7];   /**< 6 parts, total */
    budyk_entity_prev_c disk_prev[BUDYK_MAX_ENTITIES];
    uint64_t            disk_prev_ns;
    budyk_entity_prev_c iface_prev[BUDYK_MAX_ENTITIES];
    uint64_t            iface_prev_ns;
    /** Processes: the previous read and the one being made, by pid
     * (proc_track[proc_cur] is the previous), and the candidates. */
    budyk_proc_prev_c   proc_track[2][BUDYK_MAX_TRACKED_PROCS];
    uint64_t            proc_prev_ns;
    uint64_t            proc_now_ns;
    budyk_process_c     top_cpu[BUDYK_TOP_EACH];
    budyk_process_c     top_rss[BUDYK_TOP_EACH];
    uint8_t             core_has_prev[BUDYK_MAX_CORES];
    uint8_t             proc_cur;
} budyk_detail_c;

/** The process tracker (src/collector/detail.c), driven by the platform
 * process collectors: begin, add each process read, end. CPU% is the
 * change in a process's CPU time since the previous read (same pid and
 * start time) over the time between the reads; a process seen for the
 * first time, or one past BUDYK_MAX_TRACKED_PROCS, reads 0.
 * `cpu_per_sec` is the unit of budyk_proc_read_c::cpu: clock ticks per
 * second (Linux), 1000000 (FreeBSD, microseconds). */
void budyk_procs_begin(budyk_detail_c* d, uint64_t now_ns);
void budyk_procs_add  (budyk_detail_c* d, const budyk_proc_read_c* p, double cpu_per_sec);
void budyk_procs_end  (budyk_detail_c* d);

/** The previous counters of entity `name` in `prev` (n entries), or NULL
 * if it had none: a device that just appeared gets a baseline, not a
 * rate. src/collector/detail.c. */
const budyk_entity_prev_c* budyk_find_prev(const budyk_entity_prev_c* prev, uint32_t n,
                                           const char* name);
/** Per-second rate of a cumulative counter between two reads `dns` apart;
 * 0 when there is no previous read or the counter went backwards. */
double budyk_rate(uint64_t now, const uint64_t* prev, uint64_t dns);
/** The same, whole, for the byte rates: computed in double, since
 * delta x 1e9 overflows 64 bits above 18.4 GB per interval (61 MB/s over
 * an L1 interval of 5 minutes); UINT64_MAX at most. */
uint64_t budyk_rate_u64(uint64_t now, const uint64_t* prev, uint64_t dns);
/** Add what device `name`'s two counters (a, b) moved since the previous
 * call to *moved_a and *moved_b: its own counters against its own last
 * values in `prev` (nprev entries). A device not there last time, or
 * whose counter went back (re-created), moves nothing this time, so one
 * that comes or goes does not move the total by its whole counter. Its
 * counters go into next[*nnext] for the next call (BUDYK_MAX_COUNTED at
 * most; past that a device moves nothing). *pos is where in prev to look
 * first: 0 before the first device of a read, then left as this call
 * sets it (after the entry it found), so a read in the same order as the
 * last one finds each device at once. src/collector/detail.c. */
void budyk_counted_add(const budyk_counted_c* prev, uint32_t nprev, uint32_t* pos,
                       budyk_counted_c* next, uint32_t* nnext, const char* name,
                       uint64_t a, uint64_t b, uint64_t* moved_a, uint64_t* moved_b);

int budyk_collect_cpu_linux(budyk_cpu_ctx_c* ctx, budyk_sample_c* s);
/// The same, filling d->cores too when d is not NULL (the _ex collectors
/// below likewise fill their part of the detail).
int budyk_collect_cpu_linux_ex(budyk_cpu_ctx_c* ctx, budyk_sample_c* s, budyk_detail_c* d);

/// Disk / network collection also needs across-tick state — both accumulate
/// monotonic byte counters and compute per-second rates from the delta.
/// Caller must set s->timestamp_nanos before the call; collector uses the
/// elapsed wall-clock since the previous call to divide the byte delta.
/// First call records the baseline and sets rates to 0. The total is the
/// sum of each device's own delta (budyk_counted_add()): the delta of
/// the summed counters jumped by a whole counter when an interface or a
/// disk came (a veth, a jail's vnet back on the host) and read 0 when one
/// went.
typedef struct {
    budyk_counted_c dev[BUDYK_MAX_COUNTED];   ///< each whole disk's bytes read, written
    uint32_t        ndev;
    uint64_t        prev_ns;
    int             has_prev;
} budyk_disk_ctx_c;

typedef struct {
    budyk_counted_c dev[BUDYK_MAX_COUNTED];   ///< each non-loopback interface's bytes rx, tx
    uint32_t        ndev;
    uint64_t        prev_ns;
    int             has_prev;
} budyk_net_ctx_c;

int budyk_collect_disk_linux   (budyk_disk_ctx_c* ctx, budyk_sample_c* s);
int budyk_collect_network_linux(budyk_net_ctx_c*  ctx, budyk_sample_c* s);
int budyk_collect_disk_linux_ex   (budyk_disk_ctx_c* ctx, budyk_sample_c* s, budyk_detail_c* d);
int budyk_collect_network_linux_ex(budyk_net_ctx_c*  ctx, budyk_sample_c* s, budyk_detail_c* d);
int budyk_collect_proc_linux   (budyk_sample_c* s);
/// The same, and the top processes in d (not kernel threads) when d is
/// not NULL.
int budyk_collect_proc_linux_ex(budyk_sample_c* s, budyk_detail_c* d);
/// Parsers behind the collectors, exported for the fixture tests.
/// One `/proc/<pid>/stat` line: its state letter and num_threads; or all
/// the tracker needs, and the process flags (PF_KTHREAD: 0x00200000).
int  budyk_linux_parse_proc_stat(const char* line, char* state, uint32_t* threads);
int  budyk_linux_parse_proc_stat_ex(const char* line, budyk_proc_read_c* out, uint32_t* flags);
/// Undo the octal escapes (\040 ...) of a /proc/self/mounts field, in place.
void budyk_linux_unescape_mount(char* s);
int budyk_collect_entropy_linux(budyk_sample_c* s);
int budyk_collect_self_linux   (budyk_sample_c* s);
int budyk_collect_thermal_linux(budyk_sample_c* s);
int budyk_collect_netstat_linux(budyk_netstat_ctx_c* ctx, budyk_sample_c* s);
/// Where the CPU, disk, network, netstat, process
/// and filesystem collectors read /proc
/// from: tests point them at a fixture tree; NULL restores "/proc".
void        budyk_linux_set_proc_root(const char* root);
const char* budyk_linux_proc_root(void);
/// The same for /sys, where the network collector reads each
/// interface's flags and whether it is a device; NULL restores "/sys".
void        budyk_linux_set_sys_root(const char* root);
const char* budyk_linux_sys_root(void);
int budyk_collect_fs_linux     (budyk_sample_c* s);
int budyk_collect_fs_linux_ex  (budyk_sample_c* s, budyk_detail_c* d);
int budyk_collect_clock_linux  (budyk_sample_c* s);

/// FreeBSD collectors. Same shape as Linux ones; built only when
/// BUDYK_PLATFORM=freebsd. Returns 0 on success, negative errno on failure.
int budyk_collect_memory_freebsd (budyk_sample_c* s);
int budyk_collect_load_freebsd   (budyk_sample_c* s);
int budyk_collect_uptime_freebsd (budyk_sample_c* s);
int budyk_collect_cpu_freebsd    (budyk_cpu_ctx_c*  ctx, budyk_sample_c* s);
int budyk_collect_disk_freebsd   (budyk_disk_ctx_c* ctx, budyk_sample_c* s);
int budyk_collect_network_freebsd(budyk_net_ctx_c*  ctx, budyk_sample_c* s);
int budyk_collect_cpu_freebsd_ex    (budyk_cpu_ctx_c*  ctx, budyk_sample_c* s, budyk_detail_c* d);
int budyk_collect_disk_freebsd_ex   (budyk_disk_ctx_c* ctx, budyk_sample_c* s, budyk_detail_c* d);
int budyk_collect_network_freebsd_ex(budyk_net_ctx_c*  ctx, budyk_sample_c* s, budyk_detail_c* d);
/** Whether an interface of if_data type `type` whose driver and unit are
 *  `driver` (IFDATA_DRIVERNAME, "epair3" for one renamed e0a_web) is a
 *  device's, whose traffic the network totals count: Ethernet or 802.11,
 *  and the driver (the letters before the unit) none of `cloners`
 *  (`ncloners` names of IFNAMSIZ bytes each, as SIOCIFGCLONERS gives
 *  them) but wlan, and not netgraph's ngeth. */
int budyk_freebsd_iface_physical(const char* driver, unsigned int type,
                                 const char* cloners, uint32_t ncloners);
int budyk_collect_proc_freebsd   (budyk_sample_c* s);
int budyk_collect_proc_freebsd_ex(budyk_sample_c* s, budyk_detail_c* d);
int budyk_collect_entropy_freebsd(budyk_sample_c* s);
int budyk_collect_self_freebsd   (budyk_sample_c* s);
int budyk_collect_thermal_freebsd(budyk_sample_c* s);
int budyk_collect_netstat_freebsd(budyk_netstat_ctx_c* ctx, budyk_sample_c* s);
int budyk_collect_fs_freebsd     (budyk_sample_c* s);
int budyk_collect_fs_freebsd_ex  (budyk_sample_c* s, budyk_detail_c* d);
int budyk_collect_clock_freebsd  (budyk_sample_c* s);

#ifdef __cplusplus
}
#endif
#endif // BUDYK_SAMPLE_C_H
