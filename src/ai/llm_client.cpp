// SPDX-License-Identifier: BSD-3-Clause
#include "ai/llm_client.h"

#include "core/json_text.h"
#include "util/tmpfile.h"

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
    return write_private_tmp("budyk_llm_", body, path_out, cap);
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

std::string llm_escape_json(const std::string& s) { return json_escape(s); }

std::string llm_unescape_json(const std::string& s) { return json_unescape(s); }

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
