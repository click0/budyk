// SPDX-License-Identifier: BSD-3-Clause
#pragma once

namespace budyk {

// `budyk serve [--config PATH] [--enable-exec] [--enable-freeze]`: the
// daemon. Returns the process exit code. `version` is what /api/health
// reports.
int cmd_serve(int argc, char* argv[], const char* version);

} // namespace budyk
