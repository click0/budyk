// SPDX-License-Identifier: BSD-3-Clause
#include "core/json_text.h"

#include <cstdio>

namespace budyk {

namespace {

// The length of the well-formed UTF-8 sequence at s[i] (RFC 3629: no
// overlong forms, no surrogates, nothing past U+10FFFF), or 0.
size_t utf8_len(const std::string& s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    size_t n = 0;
    unsigned char lo = 0x80, hi = 0xBF;          // the second byte's range
    if      (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c == 0xE0)              { n = 3; lo = 0xA0; }
    else if (c == 0xED)              { n = 3; hi = 0x9F; }
    else if (c >= 0xE1 && c <= 0xEF) n = 3;              // 0xED taken above
    else if (c == 0xF0)              { n = 4; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) n = 4;
    else if (c == 0xF4)              { n = 4; hi = 0x8F; }
    else return 0;
    if (i + n > s.size() || b(i + 1) < lo || b(i + 1) > hi) return 0;
    for (size_t k = 2; k < n; ++k) {
        if (b(i + k) < 0x80 || b(i + k) > 0xBF) return 0;
    }
    return n;
}

} // namespace

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (static_cast<unsigned char>(c) >= 0x80) {
            // Text from outside (a process name, a mount point) can be
            // any bytes; JSON and a WebSocket text frame must be UTF-8.
            const size_t n = utf8_len(s, i);
            if (n == 0) {
                out += "\\ufffd";
            } else {
                out.append(s, i, n);
                i += n - 1;
            }
            continue;
        }
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string json_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i++];
            continue;
        }
        const char c = s[i + 1];
        switch (c) {
            case '"':  out += '"';  i += 2; break;
            case '\\': out += '\\'; i += 2; break;
            case '/':  out += '/';  i += 2; break;
            case 'b':  out += '\b'; i += 2; break;
            case 'f':  out += '\f'; i += 2; break;
            case 'n':  out += '\n'; i += 2; break;
            case 'r':  out += '\r'; i += 2; break;
            case 't':  out += '\t'; i += 2; break;
            case 'u': {
                if (i + 5 >= s.size()) { out += s[i++]; break; }
                unsigned code = 0;
                for (size_t k = 0; k < 4; ++k) {
                    const char h = s[i + 2 + k];
                    code <<= 4;
                    if      (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    else { code = 0xFFFD; break; }
                }
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                i += 6;
                break;
            }
            default:   out += c;    i += 2; break;
        }
    }
    return out;
}

} // namespace budyk
