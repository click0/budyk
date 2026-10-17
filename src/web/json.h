// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/json_text.h"
#include "core/sample.h"
#include "core/sample_c.h"

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

// Serialise one Sample as a JSON object: ts, level, cpu, mem, swap,
// load, disk, net, proc, entropy, self, thermal, file_watch, tcp, udp,
// fs, clock, uptime_seconds, and "folded":true on a record folded from
// finer samples (TierAggregator). No trailing newline.
std::string sample_to_json(const Sample& s);

// The per-entity detail of the newest sample: {"cores":[...],
// "disks":[...],"interfaces":[...],"filesystems":[...]}, names escaped.
std::string detail_to_json(const budyk_detail_c& d);

// Serialise a span of samples as the body of /api/samples:
// {"count":N,"samples":[<obj>,<obj>,...]}, plus "detail":<detail_json>
// when that is given and not empty (the newest sample's detail).
std::string samples_to_json(const Sample* samples, size_t n,
                            const std::string* detail_json = nullptr);

// Pull the value of a top-level string field out of a small JSON
// object: `"<key>" : "<value>"`, with the value's escapes decoded.
// Enough for the login form's {"password": "..."}; this is not a JSON
// parser. Returns false if the key is missing or the value isn't a
// string.
bool json_get_string(const std::string& body, const char* key, std::string* out);

} // namespace budyk
