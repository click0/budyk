/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/linux/netstat.c — TCP / UDP counters from /proc/net/snmp
 * and /proc/net/sockstat (all address families: the kernel's TCP and
 * UDP MIB counters are shared by IPv4 and IPv6). */

#include "core/sample_c.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* /proc/net/snmp comes in pairs of lines per protocol, names then
 * values:
 *   Tcp: RtoAlgorithm RtoMin ... ActiveOpens PassiveOpens ... CurrEstab InSegs ...
 *   Tcp: 1 200 ... 1234 567 ... 12 345678 ...
 * The columns are looked up by name, so a kernel that adds one does
 * not shift the others. Returns 1 if `want` was found in the pair for
 * `proto` ("Tcp:"), with its value in *out. */
static int snmp_value(const char* names, const char* values,
                      const char* want, unsigned long long* out) {
    char nbuf[1024], vbuf[1024];
    snprintf(nbuf, sizeof(nbuf), "%s", names);
    snprintf(vbuf, sizeof(vbuf), "%s", values);
    char* nsave = NULL;
    char* vsave = NULL;
    char* n = strtok_r(nbuf, " \n", &nsave);
    char* v = strtok_r(vbuf, " \n", &vsave);
    while (n != NULL && v != NULL) {
        if (strcmp(n, want) == 0) {
            char* end = NULL;
            *out = strtoull(v, &end, 10);
            return end != v;
        }
        n = strtok_r(NULL, " \n", &nsave);
        v = strtok_r(NULL, " \n", &vsave);
    }
    return 0;
}

/* Per-second rate of a cumulative counter; 0 when it went backwards
 * (a counter reset, or a network namespace switch). */
static double rate(unsigned long long now, uint64_t prev, uint64_t dns) {
    if (now < prev || dns == 0) return 0.0;
    return (double)(now - prev) * 1.0e9 / (double)dns;
}

int budyk_collect_netstat_linux(budyk_netstat_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;
    memset(&s->tcp, 0, sizeof(s->tcp));
    memset(&s->udp, 0, sizeof(s->udp));

    char path[512];
    snprintf(path, sizeof(path), "%s/net/snmp", budyk_linux_proc_root());
    FILE* f = fopen(path, "r");
    if (f == NULL) return -errno;
    char tcp_n[1024] = "", tcp_v[1024] = "", udp_n[1024] = "", udp_v[1024] = "";
    char line[1024];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "Tcp: ", 5) == 0) {
            if (tcp_n[0] == '\0') snprintf(tcp_n, sizeof(tcp_n), "%s", line + 5);
            else                  snprintf(tcp_v, sizeof(tcp_v), "%s", line + 5);
        } else if (strncmp(line, "Udp: ", 5) == 0) {
            if (udp_n[0] == '\0') snprintf(udp_n, sizeof(udp_n), "%s", line + 5);
            else                  snprintf(udp_v, sizeof(udp_v), "%s", line + 5);
        }
    }
    fclose(f);

    unsigned long long act = 0, pas = 0, estab = 0, in = 0, out = 0, retr = 0, inerr = 0;
    const int tcp_ok =
        snmp_value(tcp_n, tcp_v, "ActiveOpens",  &act)   &&
        snmp_value(tcp_n, tcp_v, "PassiveOpens", &pas)   &&
        snmp_value(tcp_n, tcp_v, "CurrEstab",    &estab) &&
        snmp_value(tcp_n, tcp_v, "InSegs",       &in)    &&
        snmp_value(tcp_n, tcp_v, "OutSegs",      &out)   &&
        snmp_value(tcp_n, tcp_v, "RetransSegs",  &retr);
    snmp_value(tcp_n, tcp_v, "InErrs", &inerr);       /* missing on very old kernels */

    unsigned long long uin = 0, uout = 0, unoport = 0, uerr = 0;
    const int udp_ok =
        snmp_value(udp_n, udp_v, "InDatagrams",  &uin)  &&
        snmp_value(udp_n, udp_v, "OutDatagrams", &uout) &&
        snmp_value(udp_n, udp_v, "NoPorts",      &unoport) &&
        snmp_value(udp_n, udp_v, "InErrors",     &uerr);

    /* TIME_WAIT sockets: /proc/net/sockstat, "TCP: inuse N orphan N tw N ...". */
    unsigned long long tw = 0;
    snprintf(path, sizeof(path), "%s/net/sockstat", budyk_linux_proc_root());
    f = fopen(path, "r");
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            const char* p = strncmp(line, "TCP: ", 5) == 0 ? strstr(line, " tw ") : NULL;
            if (p != NULL) tw = strtoull(p + 4, NULL, 10);
        }
        fclose(f);
    }

    const int      have_prev = ctx->has_prev && s->timestamp_nanos > ctx->prev_ns;
    const uint64_t dns       = have_prev ? s->timestamp_nanos - ctx->prev_ns : 0;
    if (tcp_ok) {
        s->tcp.present     = 1;
        s->tcp.established = (uint32_t)estab;
        s->tcp.time_wait   = (uint32_t)tw;
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
            s->udp.in_datagrams_per_sec  = rate(uin,     ctx->udp_in,        dns);
            s->udp.out_datagrams_per_sec = rate(uout,    ctx->udp_out,       dns);
            s->udp.no_port_per_sec       = rate(unoport, ctx->udp_no_port,   dns);
            s->udp.in_errors_per_sec     = rate(uerr,    ctx->udp_in_errors, dns);
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
    ctx->udp_no_port       = unoport;
    ctx->udp_in_errors     = uerr;
    ctx->prev_ns           = s->timestamp_nanos;
    ctx->has_prev          = 1;
    return tcp_ok || udp_ok ? 0 : -ENODATA;
}
