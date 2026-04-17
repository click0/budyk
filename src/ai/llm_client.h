// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Optional (cmake -DENABLE_LLM=ON): send anonymized metric summaries to LLM API.
// Tier B — user-initiated, opt-in.
#ifdef BUDYK_LLM_ENABLED
namespace budyk {
int suggest_rules_llm(const char* api_key, const char* summary, char* lua_buf, int buf_size);
} // namespace budyk
#endif
