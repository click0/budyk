// SPDX-License-Identifier: BSD-3-Clause
#pragma once
namespace budyk {
// Generate suggested-rules.lua from baseline statistics (Tier A — local, no network)
int suggest_rules(const char* data_dir, int window_days, const char* output_path);
} // namespace budyk
