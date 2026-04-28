// SPDX-License-Identifier: BSD-3-Clause
#include "ai/llm_client.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace budyk {

namespace {

constexpr const char* kEndpoint  = "https://api.anthropic.com/v1/messages";
constexpr const char* kModel     = "claude-haiku-4-5-20251001";
constexpr const char* kVersion   = "2023-06-01";
constexpr int         kMaxTokens = 1500;

constexpr const char* kSystemPrompt =
    "You are an SRE assistant. Given the metric baseline statistics "
    "below, suggest 3-5 Lua watch() rules for the budyk rule engine. "
    "Use only fields from cpu / mem / swap / load / disk / net. Each "
    "watch() must include `when`, `severity`, and `action`. Reply with "
    "ONLY Lua code wrapped in a ```lua block, no explanation prose.\n"
    "Available fields:\n"
    "  cpu.total_percent, cpu.count\n"
    "  mem.total, mem.available, mem.available_percent\n"
    "  swap.total, swap.used, swap.used_percent\n"
    "  load.avg_1m, load.avg_5m, load.avg_15m\n"
    "  disk.read_bytes_per_sec, disk.write_bytes_per_sec, disk.device_count\n"
    "  net.rx_bytes_per_sec, net.tx_bytes_per_sec, net.interface_count\n"
    "  uptime_seconds\n";

bool write_tmp(const std::string& body, char* path_out, size_t cap) {
    if (cap < 32) return false;
    std::strcpy(path_out, "/tmp/budyk_llm_XXXXXX");
    int fd = ::mkstemp(path_out);
    if (fd < 0) return false;
    const ssize_t n = ::write(fd, body.data(), body.size());
    ::close(fd);
    return n == static_cast<ssize_t>(body.size());
}

// Strip a Lua code fence ("```lua" ... "```") if present; otherwise
// return the input unchanged.
std::string strip_fences(const std::string& s) {
    auto open = s.find("```");
    if (open == std::string::npos) return s;
    auto eol = s.find('\n', open);
    if (eol == std::string::npos) return s;
    auto close = s.find("```", eol);
    if (close == std::string::npos) return s.substr(eol + 1);
    return s.substr(eol + 1, close - eol - 1);
}

} // namespace

std::string llm_escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
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

std::string llm_unescape_json(const std::string& s) {
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
                for (int k = 0; k < 4; ++k) {
                    const char h = s[i + 2 + k];
                    code <<= 4;
                    if      (h >= '0' && h <= '9') code |= (h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
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

std::string llm_extract_response_text(const std::string& body) {
    // Anthropic /v1/messages response shape:
    //   {"content":[{"type":"text","text":"..."}, ...], ...}
    // Pick the first "text":"<value>" — sufficient for the Tier-B
    // suggester since we ask for a single text block.
    auto key = body.find("\"text\":");
    if (key == std::string::npos) return {};
    auto open = body.find('"', key + 7);
    if (open == std::string::npos) return {};

    size_t i = open + 1;
    while (i < body.size()) {
        if (body[i] == '\\' && i + 1 < body.size()) { i += 2; continue; }
        if (body[i] == '"')                          break;
        ++i;
    }
    if (i >= body.size()) return {};
    return llm_unescape_json(body.substr(open + 1, i - (open + 1)));
}

int suggest_rules_llm(const std::string& api_key,
                      const std::string& summary,
                      std::string*       out) {
    if (api_key.empty() || out == nullptr) return -1;

    std::string content = kSystemPrompt;
    content += "\nMetric summary:\n";
    content += summary;

    std::string body;
    body.reserve(content.size() + 256);
    body += "{\"model\":\"";
    body += kModel;
    body += "\",\"max_tokens\":";
    {
        char tk[16];
        std::snprintf(tk, sizeof(tk), "%d", kMaxTokens);
        body += tk;
    }
    body += ",\"messages\":[{\"role\":\"user\",\"content\":\"";
    body += llm_escape_json(content);
    body += "\"}]}";

    // Drop body + headers in temp files so the API key never lands on
    // any process command line (visible via `ps`).
    char body_path[64];
    if (!write_tmp(body, body_path, sizeof(body_path))) return -2;

    std::string headers;
    headers += "x-api-key: ";        headers += api_key;  headers += "\n";
    headers += "anthropic-version: "; headers += kVersion; headers += "\n";
    headers += "content-type: application/json\n";
    char hdr_path[64];
    if (!write_tmp(headers, hdr_path, sizeof(hdr_path))) {
        ::unlink(body_path);
        return -2;
    }

    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
        "curl -sS -X POST -H @%s -d @%s --max-time 60 %s 2>&1",
        hdr_path, body_path, kEndpoint);

    FILE* p = ::popen(cmd, "r");
    if (p == nullptr) {
        ::unlink(body_path); ::unlink(hdr_path);
        return -3;
    }
    std::string resp;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) {
        resp.append(buf, n);
    }
    const int rc = ::pclose(p);
    ::unlink(body_path);
    ::unlink(hdr_path);
    if (rc != 0) return -4;

    std::string text = llm_extract_response_text(resp);
    if (text.empty()) return -5;
    *out = strip_fences(text);
    return 0;
}

} // namespace budyk
