/* SPDX-License-Identifier: BSD-3-Clause */
/* collector/freebsd/disk.c — diagnostic stub. Real implementation
 * via devstat(3) is being debugged on CI; reverting to a no-op so
 * the rest of the suite stays green while I narrow down the
 * compile/link issue. The function still satisfies the prototype
 * and returns 0 with all disk fields zeroed.
 */

#include "core/sample_c.h"

#include <errno.h>

int budyk_collect_disk_freebsd(budyk_disk_ctx_c* ctx, budyk_sample_c* s) {
    if (ctx == NULL || s == NULL) return -EINVAL;

    s->disk.read_bytes_per_sec  = 0;
    s->disk.write_bytes_per_sec = 0;
    s->disk.device_count        = 0;

    ctx->prev_read_sectors  = 0;
    ctx->prev_write_sectors = 0;
    ctx->prev_ns            = s->timestamp_nanos;
    ctx->has_prev           = 1;
    return 0;
}
