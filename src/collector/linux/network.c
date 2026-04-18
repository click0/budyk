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

int budyk_collect_network_linux(budyk_net_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;

    FILE* f = fopen("/proc/net/dev", "r");
    if (f == NULL) return -errno;

    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
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

        rx_bytes += rxb;
        tx_bytes += txb;
        ++iface_count;
    }
    fclose(f);

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
