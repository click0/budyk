// SPDX-License-Identifier: BSD-3-Clause
// Tier-B LLM client: the JSON escape / unescape / extract helpers, and
// the curl call against a stand-in `curl` first in PATH (a live call
// needs an API key and is tried by hand with `budyk suggest-rules --ai`).

#include "ai/llm_client.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace budyk;

namespace {

std::string slurp(const std::string& path) {
    std::string s;
    FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

size_t entries(const std::string& dir) {
    size_t n = 0;
    DIR* d = ::opendir(dir.c_str());
    assert(d != nullptr);
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") ++n;
    }
    ::closedir(d);
    return n;
}

// The stand-in: records its argv as [arg][arg]..., the -H file (the
// real one is unlinked once curl exits) and that file's mode, then acts
// out $FAKE_CURL_MODE: a good response, a curl failure, an API error.
constexpr const char* kFakeCurl = R"SH(#!/bin/sh
for a in "$@"; do printf '[%s]' "$a" >> 'REC'; done
printf '\n' >> 'REC'
out=; err=; hdr=
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out=$2; shift ;;
    --stderr) err=$2; shift ;;
    -H) hdr=${2#@}; shift ;;
  esac
  shift
done
printf 'HDR:' >> 'REC'; cat "$hdr" >> 'REC'
printf 'MODE:%s\n' "$(stat -c %a "$hdr" 2>/dev/null || stat -f %Lp "$hdr")" >> 'REC'
case "$FAKE_CURL_MODE" in
  ok)     printf '%s' '{"content":[{"type":"text","text":"```lua\nwatch(\"x\", {})\n```"}]}' > "$out" ;;
  fail)   echo 'curl: (6) Could not resolve host: api.anthropic.com' > "$err"; exit 6 ;;
  apierr) printf '%s' '{"type":"error","error":{"type":"authentication_error"}}' > "$out" ;;
esac
)SH";

} // namespace

int main() {
    // 1. Escape the obvious reservations.
    {
        assert(llm_escape_json("plain text") == "plain text");
        assert(llm_escape_json("a\"b\\c")    == "a\\\"b\\\\c");
        assert(llm_escape_json("line1\nline2") == "line1\\nline2");
        assert(llm_escape_json("\t\r\b\f")   == "\\t\\r\\b\\f");
    }

    // 2. Sub-0x20 control characters → \uXXXX.
    {
        std::string ctrl = std::string("\x01\x1f");
        std::string out = llm_escape_json(ctrl);
        assert(out == "\\u0001\\u001f");
    }

    // 3. Unescape — round-trip with escape on a non-trivial Lua snippet.
    {
        const std::string original =
            "watch(\"high_cpu\", {\n"
            "  when     = function() return cpu.total_percent > 85 end,\n"
            "  severity = \"warning\",\n"
            "  action   = alert,\n"
            "})\n";
        const std::string round = llm_unescape_json(llm_escape_json(original));
        assert(round == original);
    }

    // 4. \u escape decodes ASCII / BMP correctly.
    {
        assert(llm_unescape_json("\\u0041\\u0042\\u0043")     == "ABC");
        assert(llm_unescape_json("\\u00e9")                    == "\xc3\xa9");      // é
        // Lone surrogate or BMP non-ASCII: just verify it's UTF-8 multi-byte.
        const std::string out = llm_unescape_json("\\u4e2d");                       // 中
        assert(out.size() == 3);
        assert(static_cast<unsigned char>(out[0]) == 0xe4);
        assert(static_cast<unsigned char>(out[1]) == 0xb8);
        assert(static_cast<unsigned char>(out[2]) == 0xad);
    }

    // 5. Extract — the first "text":"..." in a synthetic Anthropic response.
    {
        const std::string body = R"({"id":"x","content":[{"type":"text","text":"hello \"world\"\nbye"},{"type":"text","text":"unused"}]})";
        const std::string extracted = llm_extract_response_text(body);
        assert(extracted == "hello \"world\"\nbye");
    }

    // 6. Extract — missing field returns empty.
    {
        assert(llm_extract_response_text("{}").empty());
        assert(llm_extract_response_text("{\"id\":\"x\"}").empty());
        assert(llm_extract_response_text("garbage").empty());
    }

    // 7. Extract — escaped backslash in the value is unescaped.
    {
        const std::string body = R"({"content":[{"text":"a\\b\\c"}]})";
        assert(llm_extract_response_text(body) == "a\\b\\c");
    }

    // 8. suggest_rules_llm guards on empty key.
    {
        std::string out, why;
        assert(suggest_rules_llm("", "summary", &out, &why) == -1);
        assert(why == "no API key");
        assert(suggest_rules_llm("k", "summary", nullptr) == -1);
    }

    // 9. The curl call, against the stand-in. curl runs without a shell:
    //    TMPDIR is long (past the old 64-byte path buffers) and full of
    //    shell syntax, and still reaches curl as one argument per file,
    //    with nothing run. The key is in the 0600 header file, never in
    //    argv, and every temp file is gone afterwards. A curl failure and
    //    an API error come back as messages, not bare codes.
    if (::access("/bin/sh", X_OK) == 0) {
        char dir_tmpl[] = "/tmp/budyk_fakellm_XXXXXX";
        const char* dir_c = ::mkdtemp(dir_tmpl);
        assert(dir_c != nullptr);
        const std::string dir  = dir_c;
        const std::string bin  = dir + "/bin";
        const std::string rec  = dir + "/record";
        const std::string tmp  = dir + "/tmp dir;$(touch PWNED);`touch PWNED2`" + std::string(150, 'x');
        assert(::mkdir(bin.c_str(), 0700) == 0);
        assert(::mkdir(tmp.c_str(), 0700) == 0);
        {
            std::string script = kFakeCurl;
            for (size_t at; (at = script.find("'REC'")) != std::string::npos; )
                script.replace(at, 5, "'" + rec + "'");
            FILE* f = std::fopen((bin + "/curl").c_str(), "w");
            assert(f != nullptr);
            std::fputs(script.c_str(), f);
            std::fclose(f);
            ::chmod((bin + "/curl").c_str(), 0755);
        }
        char cwd[4096];
        assert(::getcwd(cwd, sizeof(cwd)) != nullptr);
        assert(::chdir(dir.c_str()) == 0);   // where an injected `touch` would land
        const char* old_path = std::getenv("PATH");
        const std::string saved_path = old_path != nullptr ? old_path : "";
        const char* old_tmp = std::getenv("TMPDIR");
        const std::string saved_tmp = old_tmp != nullptr ? old_tmp : "";
        ::setenv("PATH", (bin + ":" + saved_path).c_str(), 1);
        ::setenv("TMPDIR", tmp.c_str(), 1);

        std::string out, why;
        ::setenv("FAKE_CURL_MODE", "ok", 1);
        assert(suggest_rules_llm("SECRET_KEY", "summary", &out, &why) == 0);
        assert(out == "watch(\"x\", {})\n");
        const std::string record = slurp(rec);
        const std::string argv_line = record.substr(0, record.find('\n'));
        assert(!contains(argv_line, "SECRET_KEY"));
        assert(contains(argv_line, "[-o][" + tmp + "/budyk_llm_"));
        assert(contains(argv_line, "[-H][@" + tmp + "/budyk_llm_"));
        assert(contains(record, "HDR:x-api-key: SECRET_KEY\n"));
        assert(contains(record, "MODE:600\n"));
        assert(entries(tmp) == 0);                     // all four unlinked
        assert(::access("PWNED", F_OK) != 0 && ::access("PWNED2", F_OK) != 0);

        ::setenv("FAKE_CURL_MODE", "fail", 1);
        assert(suggest_rules_llm("SECRET_KEY", "summary", &out, &why) == -4);
        assert(contains(why, "status 6") && contains(why, "Could not resolve host"));
        assert(entries(tmp) == 0);

        ::setenv("FAKE_CURL_MODE", "apierr", 1);
        assert(suggest_rules_llm("SECRET_KEY", "summary", &out, &why) == -5);
        assert(contains(why, "authentication_error"));

        ::setenv("TMPDIR", (dir + "/missing").c_str(), 1);
        assert(suggest_rules_llm("SECRET_KEY", "summary", &out, &why) == -2);
        assert(contains(why, "cannot create a temp file") && contains(why, "/missing"));

        ::setenv("TMPDIR", tmp.c_str(), 1);
        ::setenv("PATH", tmp.c_str(), 1);              // no curl there
        assert(suggest_rules_llm("SECRET_KEY", "summary", &out, &why) == -3);
        assert(contains(why, "cannot run curl(1)"));

        ::setenv("PATH", saved_path.c_str(), 1);
        if (old_tmp != nullptr) ::setenv("TMPDIR", saved_tmp.c_str(), 1);
        else                    ::unsetenv("TMPDIR");
        ::unsetenv("FAKE_CURL_MODE");
        assert(::chdir(cwd) == 0);
        ::unlink((bin + "/curl").c_str());
        ::unlink(rec.c_str());
        ::rmdir(bin.c_str());
        ::rmdir(tmp.c_str());
        ::rmdir(dir.c_str());
    }

    std::printf("test_llm_client: PASS\n");
    return 0;
}
