/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/network.c — aggregate net throughput via getifaddrs(3). */

#include "core/sample_c.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <ifaddrs.h>
#include <net/if.h>

#include <errno.h>
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
    if (ctx == NULL || s == NULL) return -EINVAL;

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

        struct if_data* d = (struct if_data*)ifa->ifa_data;
        rx_bytes += (uint64_t)d->ifi_ibytes;
        tx_bytes += (uint64_t)d->ifi_obytes;
        ++iface_count;
    }
    freeifaddrs(ifap);

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
