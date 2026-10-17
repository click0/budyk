/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/network.c — aggregate net throughput via getifaddrs(3). */

#include "core/sample_c.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <ifaddrs.h>
#include <net/if.h>

#include <errno.h>
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

    uint64_t rx_bytes    = 0;
    uint64_t tx_bytes    = 0;
    uint32_t iface_count = 0;

    for (struct ifaddrs* ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)                continue;
        if (ifa->ifa_addr->sa_family != AF_LINK)  continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)        continue;
        if (ifa->ifa_data == NULL)                continue;

        const struct if_data* id = (const struct if_data*)ifa->ifa_data;
        rx_bytes += (uint64_t)id->ifi_ibytes;
        tx_bytes += (uint64_t)id->ifi_obytes;
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
                ifc->rx_bytes_per_sec   = (uint64_t)budyk_rate(now[0], &p->c[0], ddns);
                ifc->tx_bytes_per_sec   = (uint64_t)budyk_rate(now[1], &p->c[1], ddns);
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

    if (!ctx->has_prev || s->timestamp_nanos <= ctx->prev_ns) {
        s->net.rx_bytes_per_sec = 0;
        s->net.tx_bytes_per_sec = 0;
    } else {
        uint64_t drx = rx_bytes >= ctx->prev_rx_bytes
                     ? rx_bytes - ctx->prev_rx_bytes : 0;
        uint64_t dtx = tx_bytes >= ctx->prev_tx_bytes
                     ? tx_bytes - ctx->prev_tx_bytes : 0;
        uint64_t dns = s->timestamp_nanos - ctx->prev_ns;
        s->net.rx_bytes_per_sec = (drx * 1000000000ULL) / dns;
        s->net.tx_bytes_per_sec = (dtx * 1000000000ULL) / dns;
    }

    ctx->prev_rx_bytes = rx_bytes;
    ctx->prev_tx_bytes = tx_bytes;
    ctx->prev_ns       = s->timestamp_nanos;
    ctx->has_prev      = 1;
    return 0;
}
