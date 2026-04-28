// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

#include <cstddef>
#include <string>

namespace budyk {

// JSON helpers for the embedded HTTP server.
//
// Hand-rolled rather than pulling in a third-party JSON lib because (a)
// the surface is tiny, (b) the daemon already aggressively avoids
// dynamic allocation, and (c) every produced field is one of {string,
// integer, double} — the corner cases (UTF-8 escapes, Unicode planes)
// don't apply to numeric metric fields. Strings we emit (data_dir,
// version, status) come from trusted sources.

// Serialise one Sample as a JSON object: cpu, mem, swap, load, disk,
// net, ts, level, uptime_seconds. No trailing newline.
std::string sample_to_json(const Sample& s);

// Serialise a span of samples as the body of /api/samples:
// {"count":N,"samples":[<obj>,<obj>,...]}.
std::string samples_to_json(const Sample* samples, size_t n);

} // namespace budyk
