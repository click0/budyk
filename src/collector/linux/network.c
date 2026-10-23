/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/network.c — aggregate net throughput via /proc/net/dev. */

#include "core/sample_c.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* /proc/net/dev format (2 header lines, then one line per interface):
 *   ifname:  rx_bytes rx_packets rx_errs rx_drop rx_fifo rx_frame
 *            rx_compressed rx_multicast
 *            tx_bytes tx_packets tx_errs tx_drop tx_fifo tx_colls
 *            tx_carrier tx_compressed
 *
 * The loopback interface is skipped — its traffic is internal and
 * drowns out anomaly signals for real NICs.
 */

/* IFF_UP from /sys/class/net/<if>/flags (hex); 1 when it cannot be read. */
static uint8_t iface_up(const char* name) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%.64s/flags", name);
    FILE* f = fopen(path, "re");
    if (f == NULL) return 1;
    unsigned int flags = 1;
    if (fscanf(f, "%x", &flags) != 1) flags = 1;
    fclose(f);
    return (uint8_t)((flags & 0x1u) != 0);
}

int budyk_collect_network_linux(budyk_net_ctx_c* ctx, budyk_sample_c* s) {
    return budyk_collect_network_linux_ex(ctx, s, NULL);
}

/* Per interface: counters c[] = rx bytes, tx bytes, rx packets, tx
 * packets, rx errors, tx errors, rx drops. */
int budyk_collect_network_linux_ex(budyk_net_ctx_c* ctx, budyk_sample_c* s, budyk_detail_c* d) {
    if (ctx == NULL || s == NULL) return -EINVAL;
    budyk_entity_prev_c next[BUDYK_MAX_ENTITIES];
    uint32_t nnext = 0;
    const uint64_t ddns = d != NULL && d->iface_prev_ns != 0 && s->timestamp_nanos > d->iface_prev_ns
                       ? s->timestamp_nanos - d->iface_prev_ns : 0;
    if (d != NULL) d->nifaces = 0;

    char path[512];
    snprintf(path, sizeof(path), "%s/net/dev", budyk_linux_proc_root());
    FILE* f = fopen(path, "re");
    if (f == NULL) return -errno;

    budyk_counted_c counted[BUDYK_MAX_COUNTED] = {{"", 0, 0}};
    uint32_t ncounted    = 0;
    uint64_t rx_moved    = 0;
    uint64_t tx_moved    = 0;
    uint32_t iface_count = 0;

    char line[512];
    /* Discard the 2 header lines. */
    if (fgets(line, sizeof(line), f) == NULL) { fclose(f); return -ENODATA; }
    if (fgets(line, sizeof(line), f) == NULL) { fclose(f); return -ENODATA; }

    while (fgets(line, sizeof(line), f) != NULL) {
        char* colon = strchr(line, ':');
        if (colon == NULL) continue;
        *colon = '\0';

        /* Trim leading whitespace from the interface name. */
        char* name = line;
        while (*name == ' ' || *name == '\t') ++name;
        if (*name == '\0')             continue;
        if (strcmp(name, "lo") == 0)   continue;

        unsigned long long rxb, rxp, rxe, rxd, rxf, rxfr, rxc, rxm;
        unsigned long long txb, txp, txe, txd, txfi, txco, txca, txcm;
        int parsed = sscanf(colon + 1,
                            " %llu %llu %llu %llu %llu %llu %llu %llu"
                            " %llu %llu %llu %llu %llu %llu %llu %llu",
                            &rxb, &rxp, &rxe, &rxd, &rxf, &rxfr, &rxc, &rxm,
                            &txb, &txp, &txe, &txd, &txfi, &txco, &txca, &txcm);
        if (parsed < 16) continue;

        budyk_counted_add(ctx->dev, ctx->ndev, counted, &ncounted, name, rxb, txb,
                          &rx_moved, &tx_moved);
        ++iface_count;

        if (d != NULL && d->nifaces < BUDYK_MAX_ENTITIES) {
            const uint64_t now[7] = { rxb, txb, rxp, txp, rxe, txe, rxd };
            const budyk_entity_prev_c* p = budyk_find_prev(d->iface_prev, d->niface_prev, name);
            budyk_iface_c* ifc = &d->ifaces[d->nifaces++];
            memset(ifc, 0, sizeof(*ifc));
            snprintf(ifc->name, sizeof(ifc->name), "%.31s", name);
            ifc->up = iface_up(name);
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
            snprintf(n->name, sizeof(n->name), "%.31s", name);
            memcpy(n->c, now, sizeof(now));
        }
    }
    fclose(f);

    s->net.interface_count = iface_count;

    if (!ctx->has_prev || s->timestamp_nanos <= ctx->prev_ns) {
        s->net.rx_bytes_per_sec = 0;
        s->net.tx_bytes_per_sec = 0;
    } else {
        const uint64_t dns  = s->timestamp_nanos - ctx->prev_ns;
        const uint64_t zero = 0;
        s->net.rx_bytes_per_sec = budyk_rate_u64(rx_moved, &zero, dns);
        s->net.tx_bytes_per_sec = budyk_rate_u64(tx_moved, &zero, dns);
    }

    if (d != NULL) {
        memcpy(d->iface_prev, next, sizeof(next[0]) * nnext);
        d->niface_prev   = nnext;
        d->iface_prev_ns = s->timestamp_nanos;
    }
    memcpy(ctx->dev, counted, sizeof(counted[0]) * ncounted);
    ctx->ndev          = ncounted;
    ctx->prev_ns       = s->timestamp_nanos;
    ctx->has_prev      = 1;
    return 0;
}
