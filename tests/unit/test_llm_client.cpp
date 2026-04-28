// SPDX-License-Identifier: BSD-3-Clause
// Tier-B LLM client — only exercises the JSON escape / unescape /
// extract helpers. The popen path requires a live Anthropic API key
// and is tested manually via `budyk suggest-rules --ai`.

#include "ai/llm_client.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace budyk;

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
        std::string out;
        assert(suggest_rules_llm("", "summary", &out) != 0);
        assert(suggest_rules_llm("k", "summary", nullptr) != 0);
    }

    std::printf("test_llm_client: PASS\n");
    return 0;
}
