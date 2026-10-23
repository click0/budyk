/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/detail.c — helpers shared by the per-entity collectors. */

#include "core/sample_c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const budyk_entity_prev_c* budyk_find_prev(const budyk_entity_prev_c* prev, uint32_t n,
                                           const char* name) {
    for (uint32_t i = 0; i < n; ++i) {
        if (strcmp(prev[i].name, name) == 0) return &prev[i];
    }
    return NULL;
}

double budyk_rate(uint64_t now, const uint64_t* prev, uint64_t dns) {
    if (prev == NULL || dns == 0 || now < *prev) return 0.0;
    return (double)(now - *prev) * 1.0e9 / (double)dns;
}

uint64_t budyk_rate_u64(uint64_t now, const uint64_t* prev, uint64_t dns) {
    const double r = budyk_rate(now, prev, dns);
    return r >= 18446744073709551615.0 ? UINT64_MAX : (uint64_t)r;
}

void budyk_counted_add(const budyk_counted_c* prev, uint32_t nprev,
                       budyk_counted_c* next, uint32_t* nnext, const char* name,
                       uint64_t a, uint64_t b, uint64_t* moved_a, uint64_t* moved_b) {
    for (uint32_t i = 0; i < nprev; ++i) {
        if (strcmp(prev[i].name, name) != 0) continue;
        if (a >= prev[i].a) *moved_a += a - prev[i].a;
        if (b >= prev[i].b) *moved_b += b - prev[i].b;
        break;
    }
    if (*nnext < BUDYK_MAX_COUNTED) {
        budyk_counted_c* n = &next[(*nnext)++];
        snprintf(n->name, sizeof(n->name), "%.31s", name);
        n->a = a;
        n->b = b;
    }
}

/* --- The process tracker ------------------------------------------------ */

void budyk_procs_begin(budyk_detail_c* d, uint64_t now_ns) {
    d->proc_now_ns = now_ns;
    d->nproc_track[1 - d->proc_cur] = 0;
    d->ntop_cpu = d->ntop_rss = 0;
}

/* The previous read of pid `pid` started at `start`, or NULL: the
 * previous read is sorted by pid. */
static const budyk_proc_prev_c* find_proc(const budyk_detail_c* d, int32_t pid, uint64_t start) {
    const budyk_proc_prev_c* v = d->proc_track[d->proc_cur];
    uint32_t lo = 0, hi = d->nproc_track[d->proc_cur];
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (v[mid].pid < pid) lo = mid + 1; else hi = mid;
    }
    if (lo < d->nproc_track[d->proc_cur] && v[lo].pid == pid && v[lo].start == start) return &v[lo];
    return NULL;
}

/* Keep `p` in `top` (n of at most BUDYK_TOP_EACH, largest first by
 * `key`) if it is among the largest. */
static void offer(budyk_process_c* top, uint32_t* n, const budyk_process_c* p,
                  double (*key)(const budyk_process_c*)) {
    uint32_t at = *n;
    while (at > 0 && key(&top[at - 1]) < key(p)) --at;
    if (at >= BUDYK_TOP_EACH) return;
    const uint32_t last = *n < BUDYK_TOP_EACH ? *n : BUDYK_TOP_EACH - 1;
    memmove(&top[at + 1], &top[at], (size_t)(last - at) * sizeof(*top));
    top[at] = *p;
    if (*n < BUDYK_TOP_EACH) ++*n;
}

static double by_cpu(const budyk_process_c* p) { return p->cpu_percent; }
static double by_rss(const budyk_process_c* p) { return (double)p->rss_bytes; }

void budyk_procs_add(budyk_detail_c* d, const budyk_proc_read_c* r, double cpu_per_sec) {
    budyk_process_c p;
    memset(&p, 0, sizeof(p));
    p.pid       = r->pid;
    p.state     = r->state;
    p.threads   = r->threads;
    p.rss_bytes = r->rss_bytes;
    memcpy(p.name, r->name, sizeof(p.name));
    p.name[sizeof(p.name) - 1] = '\0';

    const budyk_proc_prev_c* prev = find_proc(d, r->pid, r->start);
    if (prev != NULL && cpu_per_sec > 0.0 && d->proc_now_ns > d->proc_prev_ns) {
        p.cpu_percent = budyk_rate(r->cpu, &prev->cpu, d->proc_now_ns - d->proc_prev_ns) /
                        cpu_per_sec * 100.0;
    }

    const int next = 1 - d->proc_cur;
    if (d->nproc_track[next] < BUDYK_MAX_TRACKED_PROCS) {
        budyk_proc_prev_c* t = &d->proc_track[next][d->nproc_track[next]++];
        t->pid   = r->pid;
        t->start = r->start;
        t->cpu   = r->cpu;
    }
    /* An idle process is no "top by CPU"; it may still be one by RSS. */
    if (p.cpu_percent > 0.0) offer(d->top_cpu, &d->ntop_cpu, &p, by_cpu);
    offer(d->top_rss, &d->ntop_rss, &p, by_rss);
}

static int cmp_prev(const void* a, const void* b) {
    const int32_t x = ((const budyk_proc_prev_c*)a)->pid, y = ((const budyk_proc_prev_c*)b)->pid;
    return (x > y) - (x < y);
}

static int cmp_top(const void* a, const void* b) {
    const budyk_process_c* x = (const budyk_process_c*)a;
    const budyk_process_c* y = (const budyk_process_c*)b;
    if (x->cpu_percent != y->cpu_percent) return x->cpu_percent < y->cpu_percent ? 1 : -1;
    if (x->rss_bytes   != y->rss_bytes)   return x->rss_bytes   < y->rss_bytes   ? 1 : -1;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

void budyk_procs_end(budyk_detail_c* d) {
    /* The read just made becomes the previous one, by pid (a /proc
     * listing is usually in order already). */
    const int next = 1 - d->proc_cur;
    budyk_proc_prev_c* v = d->proc_track[next];
    const uint32_t n = d->nproc_track[next];
    int sorted = 1;
    for (uint32_t i = 1; i < n && sorted; ++i) sorted = v[i - 1].pid <= v[i].pid;
    if (!sorted) qsort(v, n, sizeof(*v), cmp_prev);
    d->proc_cur     = (uint8_t)next;
    d->proc_prev_ns = d->proc_now_ns;

    /* The list: the top by CPU and by RSS, each process once. */
    d->nprocs = 0;
    for (uint32_t i = 0; i < d->ntop_cpu; ++i) d->procs[d->nprocs++] = d->top_cpu[i];
    for (uint32_t i = 0; i < d->ntop_rss; ++i) {
        int dup = 0;
        for (uint32_t k = 0; k < d->ntop_cpu && !dup; ++k) dup = d->top_cpu[k].pid == d->top_rss[i].pid;
        if (!dup) d->procs[d->nprocs++] = d->top_rss[i];
    }
    qsort(d->procs, d->nprocs, sizeof(d->procs[0]), cmp_top);
}
