// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>

namespace budyk {

// Built-in single-page web UI. The daemon serves these bytes
// verbatim at GET /, so the HTML/CSS/JS source lives entirely
// inside one C++ raw-string literal (no asset pipeline, no
// runtime filesystem dependency, no separate package).
extern const char* const kSpaIndexHtml;
extern const size_t      kSpaIndexHtmlLen;

} // namespace budyk
