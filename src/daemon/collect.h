// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"
#include "core/sample_c.h"

namespace budyk {

// One collection tick: runs every platform collector (plain C, chosen
// by BUDYK_LINUX / BUDYK_FREEBSD) into the C-shim sample and copies the
// result into `s`. Stateful collectors (CPU / disk / net) keep their
// delta context across ticks in the ctx arguments.
void collect_one(Sample* s,
                 budyk_cpu_ctx_c*  cpu_ctx,
                 budyk_disk_ctx_c* disk_ctx,
                 budyk_net_ctx_c*  net_ctx);

} // namespace budyk
