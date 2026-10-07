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
// Negative on the various failure paths, with what went wrong in
// `error` (optional) for the operator:
//   -1  invalid args (empty key / null out)
//   -2  could not write a temp file
//   -3  curl could not be started (fork failed, not on PATH)
//   -4  curl failed (network / TLS / timeout; curl's message in error)
//   -5  the response has no text field (an API error; its body in error)
int suggest_rules_llm(const std::string& api_key,
                      const std::string& summary,
                      std::string*       out,
                      std::string*       error = nullptr);

// Helpers reused by the test surface — exported so we can verify the
// JSON escape / unescape round-trip without touching the network.
std::string llm_escape_json (const std::string& s);
std::string llm_unescape_json(const std::string& s);

// Extract the first `"text": "..."` value from an Anthropic
// /v1/messages response body, JSON-unescaped. Returns empty on parse
// failure.
std::string llm_extract_response_text(const std::string& body);

} // namespace budyk
