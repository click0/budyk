/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/netstat.c — TCP / UDP counters from
 * net.inet.tcp.stats (struct tcpstat), net.inet.tcp.states (sockets per
 * TCP state) and net.inet.udp.stats (struct udpstat), as netstat -s
 * reads them. Covers IPv4 and IPv6: the stacks share these counters. */

#include "core/sample_c.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>

#include <errno.h>
#include <string.h>

/* Per-second rate of a cumulative counter; 0 when it went backwards. */
static double rate(uint64_t now, uint64_t prev, uint64_t dns) {
    if (now < prev || dns == 0) return 0.0;
    return (double)(now - prev) * 1.0e9 / (double)dns;
}

int budyk_collect_netstat_freebsd(budyk_netstat_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;
    memset(&s->tcp, 0, sizeof(s->tcp));
    memset(&s->udp, 0, sizeof(s->udp));

    struct tcpstat tcps;
    size_t len = sizeof(tcps);
    memset(&tcps, 0, sizeof(tcps));
    const int tcp_ok = sysctlbyname("net.inet.tcp.stats", &tcps, &len, NULL, 0) == 0;

    uint64_t states[TCP_NSTATES];
    size_t   slen = sizeof(states);
    memset(states, 0, sizeof(states));
    const int states_ok = sysctlbyname("net.inet.tcp.states", states, &slen, NULL, 0) == 0;

    struct udpstat udps;
    len = sizeof(udps);
    memset(&udps, 0, sizeof(udps));
    const int udp_ok = sysctlbyname("net.inet.udp.stats", &udps, &len, NULL, 0) == 0;

    const uint64_t act   = (uint64_t)tcps.tcps_connattempt;
    const uint64_t pas   = (uint64_t)tcps.tcps_accepts;
    const uint64_t retr  = (uint64_t)tcps.tcps_sndrexmitpack;
    const uint64_t in    = (uint64_t)tcps.tcps_rcvtotal;
    const uint64_t out   = (uint64_t)tcps.tcps_sndtotal;
    const uint64_t inerr = (uint64_t)tcps.tcps_rcvbadsum + (uint64_t)tcps.tcps_rcvbadoff +
                           (uint64_t)tcps.tcps_rcvshort;
    const uint64_t uin   = (uint64_t)udps.udps_ipackets;
    const uint64_t uout  = (uint64_t)udps.udps_opackets;
    const uint64_t unop  = (uint64_t)udps.udps_noport;
    const uint64_t uerr  = (uint64_t)udps.udps_hdrops + (uint64_t)udps.udps_badsum +
                           (uint64_t)udps.udps_badlen + (uint64_t)udps.udps_fullsock;

    const int      have_prev = ctx->has_prev && s->timestamp_nanos > ctx->prev_ns;
    const uint64_t dns       = have_prev ? s->timestamp_nanos - ctx->prev_ns : 0;
    if (tcp_ok) {
        s->tcp.present = 1;
        if (states_ok) {
            s->tcp.established = (uint32_t)states[TCPS_ESTABLISHED];
            s->tcp.time_wait   = (uint32_t)states[TCPS_TIME_WAIT];
        }
        if (have_prev) {
            s->tcp.active_opens_per_sec  = rate(act,   ctx->tcp_active_opens,  dns);
            s->tcp.passive_opens_per_sec = rate(pas,   ctx->tcp_passive_opens, dns);
            s->tcp.retransmits_per_sec   = rate(retr,  ctx->tcp_retransmits,   dns);
            s->tcp.in_segments_per_sec   = rate(in,    ctx->tcp_in_segments,   dns);
            s->tcp.out_segments_per_sec  = rate(out,   ctx->tcp_out_segments,  dns);
            s->tcp.in_errors_per_sec     = rate(inerr, ctx->tcp_in_errors,     dns);
        }
    }
    if (udp_ok) {
        s->udp.present = 1;
        if (have_prev) {
            s->udp.in_datagrams_per_sec  = rate(uin,  ctx->udp_in,        dns);
            s->udp.out_datagrams_per_sec = rate(uout, ctx->udp_out,       dns);
            s->udp.no_port_per_sec       = rate(unop, ctx->udp_no_port,   dns);
            s->udp.in_errors_per_sec     = rate(uerr, ctx->udp_in_errors, dns);
        }
    }

    ctx->tcp_active_opens  = act;
    ctx->tcp_passive_opens = pas;
    ctx->tcp_retransmits   = retr;
    ctx->tcp_in_segments   = in;
    ctx->tcp_out_segments  = out;
    ctx->tcp_in_errors     = inerr;
    ctx->udp_in            = uin;
    ctx->udp_out           = uout;
    ctx->udp_no_port       = unop;
    ctx->udp_in_errors     = uerr;
    ctx->prev_ns           = s->timestamp_nanos;
    ctx->has_prev          = 1;
    return tcp_ok || udp_ok ? 0 : -errno;
}
