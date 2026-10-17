/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/detail.c — helpers shared by the per-entity collectors. */

#include "core/sample_c.h"

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
