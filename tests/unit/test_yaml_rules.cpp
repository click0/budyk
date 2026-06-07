// SPDX-License-Identifier: BSD-3-Clause
// Exercises rules/yaml_compat: YAML → Lua transpilation correctness
// and an end-to-end run through LuaEngine to make sure the emitted
// snippet actually drives the existing watch() builtin.

#include "core/sample.h"
#include "rules/lua_engine.h"
#include "rules/yaml_compat.h"

#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace budyk;

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // 1. Sequence root + minimal rule (name + when) produces a watch().
    {
        const char* y =
            "- name: hot_cpu\n"
            "  when: cpu.total_percent > 90\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);
        assert(contains(lua, "watch(\"hot_cpu\""));
        assert(contains(lua, "return cpu.total_percent > 90 end"));
        // default action = alert with default severity = warning
        assert(contains(lua, "alert(\"hot_cpu\", \"warning\", \"hot_cpu\")"));
    }

    // 2. Full rule with all optional fields populated.
    {
        const char* y =
            "- name: ram_low\n"
            "  when: \"mem.available_percent < 5\"\n"
            "  for_ticks: 3\n"
            "  cooldown: 10\n"
            "  severity: critical\n"
            "  action: alert\n"
            "  message: \"RAM is gone\"\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);
        assert(contains(lua, "for_ticks = 3"));
        assert(contains(lua, "cooldown = 10"));
        assert(contains(lua, "alert(\"ram_low\", \"critical\", \"RAM is gone\")"));
    }

    // 3. action: log emits print(), not alert().
    {
        const char* y =
            "- name: tick_pulse\n"
            "  when: \"true\"\n"
            "  action: log\n"
            "  message: pulse\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);
        assert(contains(lua, "print(\"[budyk] pulse\")"));
        assert(!contains(lua, "alert("));
    }

    // 4. Mapping root with `rules:` wrapper.
    {
        const char* y =
            "rules:\n"
            "  - name: r1\n"
            "    when: \"cpu.total_percent > 0\"\n"
            "  - name: r2\n"
            "    when: \"mem.available_percent < 100\"\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);
        assert(contains(lua, "watch(\"r1\""));
        assert(contains(lua, "watch(\"r2\""));
    }

    // 5. Empty document is fine — emits no rules.
    {
        std::string lua = "X";   // sentinel to confirm we didn't append
        assert(yaml_rules_to_lua("", &lua) == 0);
        assert(lua == "X");
    }

    // 6. Bad severity falls back to warning (silent).
    {
        const char* y =
            "- name: bogus_sev\n"
            "  when: \"true\"\n"
            "  severity: not-a-thing\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);
        assert(contains(lua, "\"warning\""));
        assert(!contains(lua, "not-a-thing"));
    }

    // 7. Missing required field → error, no partial output.
    {
        const char* missing_name =
            "- when: \"true\"\n";
        std::string a;
        assert(yaml_rules_to_lua(missing_name, &a) != 0);

        const char* missing_when =
            "- name: x\n";
        std::string b;
        assert(yaml_rules_to_lua(missing_when, &b) != 0);
    }

    // 8. A newline in `when` is rejected (would let the expression
    //    close `function() return … end` early). Operators wanting
    //    real multi-line logic should drop to native Lua.
    {
        const char* y =
            "- name: sneaky\n"
            "  when: |\n"
            "    1\n"
            "    end) os.execute('rm -rf /')\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) != 0);
    }

    // 9. Malformed YAML rejected, no crash.
    {
        std::string lua;
        assert(yaml_rules_to_lua("- name: [unterminated\n", &lua) != 0);
    }

    // 10. Special chars in name/message round-trip via Lua escapes.
    {
        const char* y =
            "- name: \"path\\\"thing\"\n"
            "  when: \"true\"\n"
            "  message: \"line1\\tafter-tab\"\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);
        // Both name and message should have escaped quote / tab.
        assert(contains(lua, "path\\\"thing"));
        assert(contains(lua, "line1\\tafter-tab"));
    }

    // 11. NULL args rejected.
    {
        std::string lua;
        assert(yaml_rules_to_lua(nullptr, &lua)  != 0);
        assert(yaml_rules_to_lua("",      nullptr) != 0);
    }

    // 12. End-to-end: emitted Lua loads cleanly into the engine and
    //     the rule fires when the condition is true.
    {
        const char* y =
            "- name: always\n"
            "  when: \"true\"\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);

        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(lua.c_str()) == 0);
        assert(e.rule_count() == 1);
        Sample s{};
        assert(e.eval_tick(s) == 1);   // fired
        e.shutdown();
    }

    // 13. End-to-end: file_load_path → engine. Uses tmp file.
    {
        char tmpl[] = "/tmp/budyk_yaml_XXXXXX";
        int  fd     = ::mkstemp(tmpl);
        assert(fd >= 0);
        const char* y =
            "- name: file_ok\n"
            "  when: \"cpu.total_percent >= 0\"\n";
        const auto n = ::write(fd, y, std::strlen(y));
        (void)n;
        ::close(fd);

        std::string lua;
        assert(yaml_rules_to_lua_file(tmpl, &lua) == 0);
        assert(contains(lua, "watch(\"file_ok\""));

        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(lua.c_str()) == 0);
        Sample s{};
        assert(e.eval_tick(s) == 1);
        e.shutdown();

        ::unlink(tmpl);
    }

    // 14. file path missing → -errno (i.e. negative).
    {
        std::string lua;
        const int rc = yaml_rules_to_lua_file(
            "/tmp/budyk_yaml_does_not_exist_zz", &lua);
        assert(rc < 0);
    }

    std::printf("test_yaml_rules: PASS\n");
    return 0;
}
