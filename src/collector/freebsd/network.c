/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/network.c — aggregate net throughput via getifaddrs(3). */

#include "core/sample_c.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_mib.h>
#include <net/if_types.h>
#include <unistd.h>

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* On FreeBSD getifaddrs(3) gives one entry per (interface, address)
 * pair. The AF_LINK entries carry a `struct if_data` in ifa_data —
 * that's where the kernel keeps the per-interface byte counters
 * (ifi_ibytes / ifi_obytes), already 64-bit on every supported
 * release. We aggregate across non-loopback links and let the caller-
 * supplied timestamp drive the bytes-per-second rate calculation,
 * mirroring the Linux /proc/net/dev path.
 */
int budyk_collect_network_freebsd(budyk_net_ctx_c* ctx, budyk_sample_c* s) {
    return budyk_collect_network_freebsd_ex(ctx, s, NULL);
}

/* A device's interface (em0, vtnet0) is Ethernet or 802.11 and not one
 * a cloner made: the bridge, lagg, vlan, tun, gif and wg types are not,
 * and an epair, tap or vxlan is Ethernet but its driver (the letters
 * before the unit) is a cloner's, as is ng_eiface's ngeth, which
 * netgraph makes. wlan is a cloner too, but its traffic is the radio's.
 * `driver` is the kernel's name for it, which a rename leaves: jib and
 * Bastille rename a jail's epair0a to e0a_<jail>, iocage to vnet0.<jid>,
 * and lan0 may be em0. */
int budyk_freebsd_iface_physical(const char* driver, unsigned int type,
                                 const char* cloners, uint32_t ncloners) {
    if (type != IFT_ETHER && type != IFT_IEEE80211) return 0;
    size_t n = 0;
    while (driver[n] >= 'a' && driver[n] <= 'z') ++n;
    if (n == 4 && strncmp(driver, "wlan", 4) == 0) return 1;
    if (n == 5 && strncmp(driver, "ngeth", 5) == 0) return 0;
    for (uint32_t i = 0; i < ncloners; ++i) {
        const char* c = cloners + (size_t)i * IFNAMSIZ;
        if (strnlen(c, IFNAMSIZ) == n && strncmp(c, driver, n) == 0) return 0;
    }
    return 1;
}

/* The driver and unit of the interface of `ifa` ("epair3" for a
 * renamed e0a_web), as ifconfig -D shows it, into buf; its own name
 * when that cannot be read. */
static const char* iface_driver(const struct ifaddrs* ifa, char* buf, size_t cap) {
    unsigned short index = 0;
    memcpy(&index, (const char*)ifa->ifa_addr + offsetof(struct sockaddr_dl, sdl_index),
           sizeof(index));
    int mib[6] = { CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, (int)index,
                   IFDATA_DRIVERNAME };
    size_t len = cap;
    if (index == 0 || sysctl(mib, 6, buf, &len, NULL, 0) != 0 || len == 0) return ifa->ifa_name;
    buf[len < cap ? len : cap - 1] = '\0';
    return buf;
}

/* The kernel's interface cloners (ifconfig -C), IFNAMSIZ bytes each; 0
 * when they cannot be read. */
static uint32_t read_cloners(char* buf, uint32_t cap) {
    const int fd = socket(AF_LOCAL, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct if_clonereq req;
    memset(&req, 0, sizeof(req));
    req.ifcr_count  = (int)cap;
    req.ifcr_buffer = buf;
    uint32_t n = 0;
    if (ioctl(fd, SIOCIFGCLONERS, &req) == 0 && req.ifcr_total > 0) {
        n = (uint32_t)(req.ifcr_total < req.ifcr_count ? req.ifcr_total : req.ifcr_count);
    }
    close(fd);
    return n;
}

/* Per interface: counters c[] = rx bytes, tx bytes, rx packets, tx
 * packets, rx errors, tx errors, rx drops (input queue drops). */
int budyk_collect_network_freebsd_ex(budyk_net_ctx_c* ctx, budyk_sample_c* s, budyk_detail_c* d) {
    if (ctx == NULL || s == NULL) return -EINVAL;
    budyk_entity_prev_c next[BUDYK_MAX_ENTITIES];
    uint32_t nnext = 0;
    const uint64_t ddns = d != NULL && d->iface_prev_ns != 0 && s->timestamp_nanos > d->iface_prev_ns
                        ? s->timestamp_nanos - d->iface_prev_ns : 0;
    if (d != NULL) d->nifaces = 0;

    struct ifaddrs* ifap = NULL;
    if (getifaddrs(&ifap) != 0) return -errno;

    budyk_counted_c counted[BUDYK_MAX_COUNTED] = {{"", 0, 0}};
    uint32_t ncounted    = 0;
    uint32_t counted_pos = 0;
    uint64_t rx_moved    = 0;
    uint64_t tx_moved    = 0;
    uint64_t rx_physical = 0;
    uint64_t tx_physical = 0;
    uint32_t physical    = 0;
    uint32_t iface_count = 0;
    char     cloners[64 * IFNAMSIZ];
    const uint32_t ncloners = read_cloners(cloners, 64);

    for (struct ifaddrs* ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)                continue;
        if (ifa->ifa_addr->sa_family != AF_LINK)  continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)        continue;
        if (ifa->ifa_data == NULL)                continue;

        const struct if_data* id = (const struct if_data*)ifa->ifa_data;
        uint64_t rx = 0, tx = 0;
        budyk_counted_add(ctx->dev, ctx->ndev, &counted_pos, counted, &ncounted, ifa->ifa_name,
                          (uint64_t)id->ifi_ibytes, (uint64_t)id->ifi_obytes, &rx, &tx);
        rx_moved += rx;
        tx_moved += tx;
        char driver[64];
        if (budyk_freebsd_iface_physical(iface_driver(ifa, driver, sizeof(driver)), id->ifi_type,
                                         cloners, ncloners)) {
            rx_physical += rx;
            tx_physical += tx;
            ++physical;
        }
        ++iface_count;

        if (d != NULL && d->nifaces < BUDYK_MAX_ENTITIES) {
            const uint64_t now[7] = {
                (uint64_t)id->ifi_ibytes, (uint64_t)id->ifi_obytes,
                (uint64_t)id->ifi_ipackets, (uint64_t)id->ifi_opackets,
                (uint64_t)id->ifi_ierrors, (uint64_t)id->ifi_oerrors,
                (uint64_t)id->ifi_iqdrops,
            };
            const budyk_entity_prev_c* p = budyk_find_prev(d->iface_prev, d->niface_prev,
                                                           ifa->ifa_name);
            budyk_iface_c* ifc = &d->ifaces[d->nifaces++];
            memset(ifc, 0, sizeof(*ifc));
            snprintf(ifc->name, sizeof(ifc->name), "%.31s", ifa->ifa_name);
            ifc->up = (uint8_t)((ifa->ifa_flags & IFF_UP) != 0);
            if (p != NULL && ddns > 0) {
                ifc->rx_bytes_per_sec   = budyk_rate_u64(now[0], &p->c[0], ddns);
                ifc->tx_bytes_per_sec   = budyk_rate_u64(now[1], &p->c[1], ddns);
                ifc->rx_packets_per_sec = budyk_rate(now[2], &p->c[2], ddns);
                ifc->tx_packets_per_sec = budyk_rate(now[3], &p->c[3], ddns);
                ifc->rx_errors_per_sec  = budyk_rate(now[4], &p->c[4], ddns);
                ifc->tx_errors_per_sec  = budyk_rate(now[5], &p->c[5], ddns);
                ifc->rx_drops_per_sec   = budyk_rate(now[6], &p->c[6], ddns);
            }
            budyk_entity_prev_c* n = &next[nnext++];
            memset(n, 0, sizeof(*n));
            snprintf(n->name, sizeof(n->name), "%.31s", ifa->ifa_name);
            memcpy(n->c, now, sizeof(now));
        }
    }
    freeifaddrs(ifap);
    if (d != NULL) {
        memcpy(d->iface_prev, next, sizeof(next[0]) * nnext);
        d->niface_prev   = nnext;
        d->iface_prev_ns = s->timestamp_nanos;
    }

    s->net.interface_count = iface_count;
    /* The totals are the devices' traffic (a jail's packet crossed its
     * epair, the bridge and em0, each counting it); with none, every
     * interface's. */
    if (physical > 0) {
        rx_moved = rx_physical;
        tx_moved = tx_physical;
    }

    if (!ctx->has_prev || s->timestamp_nanos <= ctx->prev_ns) {
        s->net.rx_bytes_per_sec = 0;
        s->net.tx_bytes_per_sec = 0;
    } else {
        const uint64_t dns  = s->timestamp_nanos - ctx->prev_ns;
        const uint64_t zero = 0;
        s->net.rx_bytes_per_sec = budyk_rate_u64(rx_moved, &zero, dns);
        s->net.tx_bytes_per_sec = budyk_rate_u64(tx_moved, &zero, dns);
    }

    memcpy(ctx->dev, counted, sizeof(counted[0]) * ncounted);
    ctx->ndev          = ncounted;
    ctx->prev_ns       = s->timestamp_nanos;
    ctx->has_prev      = 1;
    return 0;
}
