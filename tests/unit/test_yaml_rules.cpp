// SPDX-License-Identifier: BSD-3-Clause
// Exercises rules/yaml_compat: YAML → Lua transpilation correctness
// and an end-to-end run through LuaEngine to make sure the emitted
// snippet actually drives the existing watch() builtin.

#include "core/sample.h"
#include "rules/lua_engine.h"
#include "rules/yaml_compat.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
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
        assert(contains(lua, "return cpu.total_percent > 90\n  end"));
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

    // 6. A severity that is none of info / warning / critical is an error
    //    naming the rule (it became warning without a word: a critical
    //    rule written `Critical` went out as a warning); any case is taken
    //    as watch() takes it. The same for an action not alert / log /
    //    escalate (anything but `log` was an alert: `escalate` escalated
    //    nothing).
    {
        const char* y =
            "- name: bogus_sev\n"
            "  when: \"true\"\n"
            "  severity: not-a-thing\n";
        std::string lua, err;
        assert(yaml_rules_to_lua(y, &lua, &err) != 0);
        assert(err == "rule 'bogus_sev': severity 'not-a-thing' is not info, warning or critical");
        assert(lua.empty());

        const char* cased =
            "- name: loud\n"
            "  when: \"true\"\n"
            "  severity: Critical\n"
            "- name: quiet\n"
            "  when: \"true\"\n"
            "  severity: INFO\n";
        assert(yaml_rules_to_lua(cased, &lua, &err) == 0 && err.empty());
        assert(contains(lua, "alert(\"loud\", \"critical\", \"loud\")"));
        assert(contains(lua, "alert(\"quiet\", \"info\", \"quiet\")"));

        const char* act =
            "- name: odd\n"
            "  when: \"true\"\n"
            "  action: notify\n";
        lua.clear();
        assert(yaml_rules_to_lua(act, &lua, &err) != 0);
        assert(err == "rule 'odd': action 'notify' is not alert, log or escalate");
    }

    // 6b. action: escalate is the spec's { alert, escalate }: the alert
    //     goes out and the level is requested.
    {
        const char* y =
            "- name: storm\n"
            "  when: \"true\"\n"
            "  severity: critical\n"
            "  action: escalate\n"
            "  message: look now\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);
        assert(contains(lua, "action = { function() alert(\"storm\", \"critical\", \"look now\") end, \"escalate\" },"));
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(lua.c_str()) == 0);
        Sample s{};
        s.groups = kGroupsAll;
        assert(e.eval_tick(s) == 1);
        assert(e.take_escalations().size() == 1);
        const auto alerts = e.take_alerts();
        assert(alerts.size() == 1 && alerts[0].message == "look now" &&
               std::string(alerts[0].severity) == "critical");
        e.shutdown();
    }

    // 6c. What is wrong, and where, in `error`: every failure used to
    //     reach the operator as "YAML parse error".
    {
        std::string lua, err;
        assert(yaml_rules_to_lua("- when: \"true\"\n", &lua, &err) != 0);
        assert(err == "rule 1: no 'name'");
        assert(yaml_rules_to_lua("- name: a\n  when: \"true\"\n- name: b\n", &lua, &err) != 0);
        assert(err == "rule 'b': no 'when'");
        assert(yaml_rules_to_lua("- name: n\n  when: \"true\"\n  for_ticks: 5s\n", &lua, &err) != 0);
        assert(err == "rule 'n': for_ticks '5s' is not a whole number of ticks");
        assert(yaml_rules_to_lua("- name: n\n  when: |\n    1\n    2\n", &lua, &err) != 0);
        assert(contains(err, "rule 'n': 'when' must be one line"));
        assert(yaml_rules_to_lua("- 7\n", &lua, &err) != 0);
        assert(err == "rule 1 is not a mapping (name:, when:, ...)");
        assert(yaml_rules_to_lua("- name: [unterminated\n", &lua, &err) != 0);
        assert(contains(err, "YAML: ") && contains(err, " at line "));
        assert(yaml_rules_to_lua("other: 1\n", &lua, &err) != 0);
        assert(err == "no 'rules:' list in the document");
        assert(yaml_rules_to_lua("rules: 3\n", &lua, &err) != 0);
        assert(err == "'rules:' is not a list");
        // Another key next to rules: was ignored without a word.
        assert(yaml_rules_to_lua("rules:\n  - {name: a, when: 'true'}\nrule:\n  - {name: b, when: 'true'}\n",
                                 &lua, &err) != 0);
        assert(err == "unknown key 'rule' next to 'rules:' (it holds the list alone)");
        assert(yaml_rules_to_lua("just a string\n", &lua, &err) != 0);
        assert(contains(err, "neither a list"));
        assert(yaml_rules_to_lua_file("/tmp/budyk_yaml_does_not_exist_zz", &lua, &err) < 0);
        assert(contains(err, "cannot read: No such file"));
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

    // 8b. for_ticks / cooldown must be whole non-negative numbers: "5s",
    //     "-1", "abc", "1.5" or past INT_MAX is an error (atoi() used to
    //     turn them into 5, nothing, 0, 1 or a wrapped value). 0 is
    //     accepted; for_ticks 0 is the default and is left out.
    {
        const char* bad[] = { "for_ticks: 5s", "for_ticks: -1", "for_ticks: abc",
                              "cooldown: 1.5", "cooldown: -1", "cooldown: ''",
                              "cooldown: 99999999999" };
        for (const char* field : bad) {
            const std::string y = std::string("- name: n\n  when: \"true\"\n  ") + field + "\n";
            std::string lua;
            assert(yaml_rules_to_lua(y.c_str(), &lua) != 0);
        }
        std::string lua;
        assert(yaml_rules_to_lua("- name: n\n  when: \"true\"\n  for_ticks: 0\n  cooldown: 0\n", &lua) == 0);
        assert(!contains(lua, "for_ticks"));
        assert(contains(lua, "cooldown = 0,"));
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
        s.groups = kGroupsAll;
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
        s.groups = kGroupsAll;
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

    // 14. Without a cooldown a YAML rule gets the engine's default, as
    //     long as its for_ticks (spec §3.6.4); cooldown: 0 is kept.
    {
        const char* y =
            "- name: slow\n"
            "  when: \"cpu.total_percent > 90\"\n"
            "  for_ticks: 3\n"
            "- name: every\n"
            "  when: \"cpu.total_percent > 90\"\n"
            "  cooldown: 0\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);
        assert(!contains(lua, "cooldown = 3"));
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(lua.c_str()) == 0);
        assert(e.rule_count() == 2);
        assert(e.rules()[0].for_ticks == 3 && e.rules()[0].cooldown_ticks == 3);
        assert(e.rules()[1].for_ticks == 1 && e.rules()[1].cooldown_ticks == 0);
        e.shutdown();
    }

    // 16. A control character before a digit keeps both: Lua's decimal
    //     escape takes up to three digits, so "\1" then "23" must not
    //     become "\123" ('{').
    {
        const char* y =
            "- name: \"a\\x0123\"\n"
            "  when: \"true\"\n";
        std::string lua;
        assert(yaml_rules_to_lua(y, &lua) == 0);
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(lua.c_str()) == 0);
        assert(e.rule_count() == 1);
        assert(e.rules()[0].name == std::string("a\x01" "23"));
        e.shutdown();
    }

    // 17. A key that is not a rule's (a typo: `cooldwon`, `serverity`,
    //     `for`), one given twice, or a list or mapping where a value
    //     belongs is an error naming the rule and the key. Each was
    //     dropped without a word: the typo's setting was lost, a
    //     `severity: [critical]` fired as a warning.
    {
        struct Case { const char* field; const char* says; };
        const Case bad[] = {
            { "  cooldwon: 5\n",            "unknown key 'cooldwon'" },
            { "  for: 5\n",                 "unknown key 'for'" },
            { "  severity: [critical]\n",   "'severity' is a list where a value belongs" },
            { "  for_ticks: { n: 5 }\n",    "'for_ticks' is a mapping where a value belongs" },
            { "  cooldown: 5\n  cooldown: 6\n", "'cooldown' given twice" },
            { "  message: ~\n  message: x\n",   "'message' given twice" },
        };
        for (const Case& c : bad) {
            const std::string y = std::string("- name: typo\n  when: \"true\"\n") + c.field;
            std::string lua, err;
            assert(yaml_rules_to_lua(y.c_str(), &lua, &err) != 0);
            assert(contains(err, "rule 'typo'") || contains(err, "rule 1"));
            assert(contains(err, c.says));
        }
    }

    // 18. A key with no value (`message:`, `~`, `null`) is one left out:
    //     the message defaults to the name (it was an empty body), the
    //     cooldown to the engine's (`cooldown: ~` was an error).
    {
        const char* y =
            "- name: quiet\n"
            "  when: \"true\"\n"
            "  message:\n"
            "  cooldown: ~\n"
            "  severity: null\n";
        std::string lua, err;
        assert(yaml_rules_to_lua(y, &lua, &err) == 0);
        assert(contains(lua, "alert(\"quiet\", \"warning\", \"quiet\")"));
        assert(!contains(lua, "cooldown ="));
        // A required key with no value is missing.
        std::string b;
        assert(yaml_rules_to_lua("- name: x\n  when: ~\n", &b, &err) != 0);
        assert(contains(err, "no 'when'"));
    }

    // 19. A `when` that does not parse is an error naming the rule and
    //     its line in the YAML file; the generated chunk's load named a
    //     line of that chunk and its first rule (`[string "watch("r1",
    //     {..."]:56: 'end' expected`).
    {
        const char* y =
            "- name: r1\n"
            "  when: \"true\"\n"
            "- name: broken\n"
            "  when: \"cpu.total_percent > \"\n";
        std::string lua, err;
        assert(yaml_rules_to_lua(y, &lua, &err) != 0);
        assert(contains(err, "rule 'broken'"));
        assert(contains(err, "(line 4)"));
        assert(contains(err, "does not parse"));
        assert(!contains(err, "r1") && !contains(err, "[string"));
        // A trailing comment in `when` no longer takes the closing `end`.
        std::string ok;
        assert(yaml_rules_to_lua("- name: c\n  when: \"true -- always\"\n", &ok, &err) == 0);
        LuaEngine e;
        assert(e.init(false) == 0);
        assert(e.load_string(ok.c_str()) == 0 && e.rule_count() == 1);
        e.shutdown();
    }

    // 20. A NUL byte in the text is refused: the text went on as a C
    //     string, and every rule after the byte was dropped in silence.
    {
        std::string y = "- name: a\n  when: \"true\"\n";
        y.push_back('\0');
        y += "- name: b\n  when: \"true\"\n";
        assert(y.size() == 51 && y[25] == '\0');
        std::string lua, err;
        assert(yaml_rules_to_lua(y, &lua, &err) != 0);
        assert(contains(err, "a NUL byte at offset 25"));
        assert(yaml_rules_to_lua(std::string(y.c_str()), &lua, &err) == 0);
    }

    // 21. The file must hold one YAML document: rules after a `---`
    //     were dropped, and a syntax error after `...` never reported.
    {
        std::string lua, err;
        assert(yaml_rules_to_lua("- name: a\n  when: \"true\"\n---\n- name: b\n  when: \"true\"\n",
                                 &lua, &err) != 0);
        assert(contains(err, "a second YAML document"));
        assert(yaml_rules_to_lua("- name: a\n  when: \"true\"\n...\ngarbage: [\n",
                                 &lua, &err) != 0);
        assert(contains(err, "YAML:"));
        // A `---` before the list, or one ending the file, is fine.
        assert(yaml_rules_to_lua("---\n- name: a\n  when: \"true\"\n---\n", &lua, &err) == 0);
    }

    // 22. The path of a directory, or of a FIFO, is an error in words:
    //     the directory was read as a file of 2^63 bytes (std::string's
    //     length_error, and the daemon aborted, at start or on SIGHUP),
    //     the FIFO held the daemon until something wrote to it.
    {
        char dir[] = "/tmp/budyk_yaml_dir_XXXXXX";
        assert(mkdtemp(dir) != nullptr);
        std::string lua, err;
        assert(yaml_rules_to_lua_file(dir, &lua, &err) == -EISDIR);
        assert(contains(err, "cannot read: Is a directory"));
        const std::string fifo = std::string(dir) + "/rules.yaml";
        assert(mkfifo(fifo.c_str(), 0600) == 0);
        assert(yaml_rules_to_lua_file(fifo.c_str(), &lua, &err) == -ENOTSUP);
        assert(contains(err, "not a regular file"));
        ::unlink(fifo.c_str());
        ::rmdir(dir);
    }

    std::printf("test_yaml_rules: PASS\n");
    return 0;
}
