// SPDX-License-Identifier: BSD-3-Clause
#include "ai/llm_client.h"

#include "core/json_text.h"
#include "rules/exec_action.h"
#include "util/tmpfile.h"

#include <cerrno>
#include <climits>
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
constexpr int         kCurlMaxTime = 60;   // seconds, curl --max-time

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

// A private temp file (see write_private_tmp), unlinked when it goes
// out of scope.
struct TmpFile {
    char path[PATH_MAX] = {};
    bool create(const std::string& body) {
        if (write_private_tmp("budyk_llm_", body, path, sizeof(path))) return true;
        path[0] = '\0';   // nothing to unlink
        return false;
    }
    TmpFile() = default;
    TmpFile(const TmpFile&) = delete;
    TmpFile& operator=(const TmpFile&) = delete;
    ~TmpFile() { if (path[0] != '\0') ::unlink(path); }
};

const char* tmp_dir() {
    const char* dir = std::getenv("TMPDIR");
    return dir != nullptr && *dir != '\0' ? dir : "/tmp";
}

// The whole of a (small) file; empty when it cannot be read.
std::string read_file(const char* path) {
    std::string s;
    FILE* f = std::fopen(path, "re");
    if (f == nullptr) return s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

// `s` on one line for an error message: line breaks become spaces,
// surrounding blanks are trimmed and the result is cut at 200 bytes.
std::string one_line(const std::string& s) {
    std::string out;
    for (const char c : s) out += (c == '\n' || c == '\r') ? ' ' : c;
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    out = out.substr(b, out.find_last_not_of(' ') - b + 1);
    if (out.size() > 200) {
        out.resize(200);
        out += "...";
    }
    return out;
}

void describe(std::string* error, const std::string& what) {
    if (error != nullptr) *error = what;
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
                      std::string*       out,
                      std::string*       error) {
    if (api_key.empty() || out == nullptr) {
        describe(error, api_key.empty() ? "no API key" : "no output buffer");
        return -1;
    }

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

    std::string headers;
    headers += "x-api-key: ";        headers += api_key;  headers += "\n";
    headers += "anthropic-version: "; headers += kVersion; headers += "\n";
    headers += "content-type: application/json\n";

    // Body and headers go through temp files so the API key is never
    // on curl's command line (readable by every local user via ps(1)).
    // curl writes the response and its own error message to two more.
    TmpFile body_f, hdr_f, resp_f, err_f;
    const char* failed = nullptr;
    if      (!body_f.create(body))    failed = "the request body";
    else if (!hdr_f.create(headers))  failed = "the request headers";
    else if (!resp_f.create(""))      failed = "the response";
    else if (!err_f.create(""))       failed = "curl's error output";
    if (failed != nullptr) {
        const int err = errno;
        describe(error, std::string("cannot create a temp file for ") + failed +
                        " in " + tmp_dir() + ": " + std::strerror(err));
        return -2;
    }

    // Run curl directly, not through a shell: the temp paths come from
    // $TMPDIR and stay one argument each whatever they contain.
    const std::string hdr_arg  = std::string("@") + hdr_f.path;
    const std::string body_arg = std::string("@") + body_f.path;
    char max_time[16];
    std::snprintf(max_time, sizeof(max_time), "%d", kCurlMaxTime);
    const char* const argv[] = {
        "curl", "-sS", "-X", "POST",
        "-H", hdr_arg.c_str(), "-d", body_arg.c_str(),
        "--max-time", max_time,
        "-o", resp_f.path, "--stderr", err_f.path,
        kEndpoint, nullptr,
    };
    ExecResult res{};
    if (exec_command(argv, kCurlMaxTime + 15, &res) != 0) {
        describe(error, std::string("cannot start curl(1): ") + std::strerror(errno));
        return -3;
    }
    if (res.timed_out) {
        describe(error, "curl(1) did not finish in " + std::to_string(kCurlMaxTime + 15) + " s");
        return -4;
    }
    if (res.signal != 0) {
        describe(error, "curl(1) was killed by signal " + std::to_string(res.signal));
        return -4;
    }
    if (res.exit_status == 127) {   // exec_command's child: execvp failed
        describe(error, "cannot run curl(1): not found on PATH?");
        return -3;
    }
    if (res.exit_status != 0) {
        std::string msg = "curl(1) exited with status " + std::to_string(res.exit_status);
        const std::string why = one_line(read_file(err_f.path));
        if (!why.empty()) msg += ": " + why;
        describe(error, msg);
        return -4;
    }

    const std::string resp = read_file(resp_f.path);
    std::string text = llm_extract_response_text(resp);
    if (text.empty()) {
        describe(error, "no text in the API response: " + one_line(resp));
        return -5;
    }
    *out = strip_fences(text);
    return 0;
}

} // namespace budyk
