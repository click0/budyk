// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>

namespace budyk {

/// @file
/// JSON string-content escaping, shared by the alert payloads, the LLM
/// client, the TUI's login body and the HTTP JSON parsing. Pure: no I/O.

/// Escape the contents of a JSON string (what goes between the quotes):
/// `\"` `\\` `\b` `\f` `\n` `\r` `\t`, and every other control character
/// as `\u00XX`. Well-formed UTF-8 passes through unchanged; a byte that
/// does not start one becomes `\ufffd`, so the result is valid UTF-8
/// whatever went in.
std::string json_escape(const std::string& s);

/// Decode the JSON escapes in a string's contents: `\"` `\\` `\/` `\b`
/// `\f` `\n` `\r` `\t` and `\uXXXX` (BMP only, emitted as UTF-8; an
/// invalid hex digit yields U+FFFD). An unknown escape keeps the
/// character after the backslash; a trailing backslash is kept.
std::string json_unescape(const std::string& s);

} // namespace budyk
