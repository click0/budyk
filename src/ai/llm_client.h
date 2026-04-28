// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <string>

namespace budyk {

// Tier B AI rule suggestions (spec §6.2). User-initiated, opt-in via
// `budyk suggest-rules --ai --api-key <KEY>`. The implementation runs
// curl(1) under the hood — see llm_client.cpp — so the daemon adds no
// link-time dependency on TLS / HTTP libraries.
//
// Returns 0 on success and writes the LLM-produced Lua rules to `out`.
// Negative on the various failure paths:
//   -1  invalid args (null / empty / buf too small)
//   -2  could not write the temp request file
//   -3  curl popen failed
//   -4  curl exited non-zero (network / API error)
//   -5  response missing the expected text field
//   -6  output buffer too small for the parsed Lua
int suggest_rules_llm(const std::string& api_key,
                      const std::string& summary,
                      std::string*       out);

// Helpers reused by the test surface — exported so we can verify the
// JSON escape / unescape round-trip without touching the network.
std::string llm_escape_json (const std::string& s);
std::string llm_unescape_json(const std::string& s);

// Extract the first `"text": "..."` value from an Anthropic
// /v1/messages response body, JSON-unescaped. Returns empty on parse
// failure.
std::string llm_extract_response_text(const std::string& body);

} // namespace budyk
