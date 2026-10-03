// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/json_text.h"
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

// Pull the value of a top-level string field out of a small JSON
// object: `"<key>" : "<value>"`, with the value's escapes decoded.
// Enough for the login form's {"password": "..."}; this is not a JSON
// parser. Returns false if the key is missing or the value isn't a
// string.
bool json_get_string(const std::string& body, const char* key, std::string* out);

} // namespace budyk
