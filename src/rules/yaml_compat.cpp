// SPDX-License-Identifier: BSD-3-Clause
#include "rules/yaml_compat.h"
#include "core/parse_int.h"
#include "rules/alert.h"
#include "util/text_file.h"
#include "util/yaml_dom.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <yaml.h>
}

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace budyk {

namespace {

// Escape a string for inclusion in a Lua "..."-quoted literal.
std::string lua_escape(const char* s) {
    std::string out;
    if (s == nullptr) return out;
    out.reserve(std::strlen(s) + 4);
    for (const char* p = s; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    // Three digits: Lua reads up to three, so "\1" before
                    // a digit would take it in ("\1" "2" is "\12").
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\%03d", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// YAML's null: a plain `~`, `null` or nothing at all (`message:` with no
// value). A key with a null value is as if it were not there.
bool is_null(const yaml_node_t* n) {
    if (n->type != YAML_SCALAR_NODE || n->data.scalar.style != YAML_PLAIN_SCALAR_STYLE) {
        return false;
    }
    const char* v = reinterpret_cast<const char*>(n->data.scalar.value);
    return *v == '\0' || std::strcmp(v, "~") == 0 || std::strcmp(v, "null") == 0 ||
           std::strcmp(v, "Null") == 0 || std::strcmp(v, "NULL") == 0;
}

// The keys a rule may have, in this order; a rule's fields by them.
const char* const kRuleKeys[] = {"name", "when", "for_ticks", "cooldown",
                                 "severity", "action", "message"};
constexpr size_t kRuleKeyCount = sizeof(kRuleKeys) / sizeof(kRuleKeys[0]);

// The rule's fields, read by walking its mapping once. A key that is
// not one of kRuleKeys (`cooldwon`, `for`), one given twice, or a list
// or mapping where a value belongs is an error: a typo dropped the
// setting without a word (`serverity: critical` fired as a warning), a
// duplicate kept the first. A null value is a key left out.
int read_rule_fields(yaml_document_t* doc, const yaml_node_t* item, const std::string& at,
                     const char* (&fields)[kRuleKeyCount], const yaml_node_t** when_node,
                     std::string* error) {
    bool seen[kRuleKeyCount] = {};
    for (auto* p = item->data.mapping.pairs.start; p != item->data.mapping.pairs.top; ++p) {
        const yaml_node_t* k = yaml_document_get_node(doc, p->key);
        const yaml_node_t* v = yaml_document_get_node(doc, p->value);
        const char* key = k != nullptr ? yaml_scalar(k) : nullptr;
        if (key == nullptr || v == nullptr) {
            *error = at + ": a key that is not a string";
            return -EINVAL;
        }
        size_t i = 0;
        while (i < kRuleKeyCount && std::strcmp(kRuleKeys[i], key) != 0) ++i;
        if (i == kRuleKeyCount) {
            *error = at + ": unknown key '" + key +
                     "' (name, when, for_ticks, cooldown, severity, action, message)";
            return -EINVAL;
        }
        if (seen[i]) {
            *error = at + ": '" + key + "' given twice";
            return -EINVAL;
        }
        seen[i] = true;
        if (is_null(v)) continue;   // a key with no value: left out ('' is a value)
        const char* s = yaml_scalar(v);
        if (s == nullptr) {
            *error = at + ": '" + key + "' is a " +
                     (v->type == YAML_SEQUENCE_NODE ? "list" : "mapping") +
                     " where a value belongs";
            return -EINVAL;
        }
        fields[i] = s;
        if (i == 1) *when_node = v;
    }
    return 0;
}

// Lua reads `when` before the file is put together: a syntax error in
// the generated chunk named a line of that chunk and its first rule
// (`[string "watch("r1", {..."]:56: 'end' expected`); here it names the
// rule and the line of the YAML file. `L` is a scratch state (nullptr
// when none could be made: the load's own check then stands).
int check_when(lua_State* L, const char* when, const yaml_node_t* when_node,
               const std::string& who, std::string* error) {
    if (L == nullptr) return 0;
    const std::string chunk = std::string("return ") + when;
    const int rc = luaL_loadbufferx(L, chunk.data(), chunk.size(), "=when", "t");
    if (rc == LUA_OK) {
        lua_pop(L, 1);
        return 0;
    }
    const char* msg = lua_tostring(L, -1);
    std::string what = msg != nullptr ? msg : "cannot parse";
    // "when:1: " is the chunk and the line, both ours.
    if (what.compare(0, 8, "when:1: ") == 0) what.erase(0, 8);
    *error = who + ": 'when' (line " + std::to_string(when_node->start_mark.line + 1) +
             ") does not parse: " + what;
    lua_pop(L, 1);
    lua_settop(L, 0);
    return -EINVAL;
}

// The rule's place in an error message: its name once it has one, else
// its position in the list (from 1).
int transpile_rule(yaml_document_t* doc, const yaml_node_t* item, size_t index,
                   lua_State* L, std::string* out, std::string* error) {
    const std::string at = "rule " + std::to_string(index);
    if (item == nullptr || item->type != YAML_MAPPING_NODE) {
        *error = at + " is not a mapping (name:, when:, ...)";
        return -EINVAL;
    }

    const char* fields[kRuleKeyCount] = {};
    const yaml_node_t* when_node = nullptr;
    if (const int rc = read_rule_fields(doc, item, at, fields, &when_node, error); rc != 0) {
        return rc;
    }
    const char* name = fields[0];
    const char* when = fields[1];
    if (name == nullptr || name[0] == '\0') {
        *error = at + ": no 'name'";
        return -EINVAL;
    }
    const std::string who = "rule '" + std::string(name) + "'";
    if (when == nullptr || when[0] == '\0') {
        *error = who + ": no 'when'";
        return -EINVAL;
    }

    // A newline in `when` would let the spliced text close `function() return …`
    // early and inject statements. Reject it — operators who really want
    // multi-line logic should drop to native Lua.
    for (const char* p = when; *p != '\0'; ++p) {
        if (*p == '\n' || *p == '\r') {
            *error = who + ": 'when' must be one line (write the rule in Lua for more)";
            return -EINVAL;
        }
    }
    if (const int rc = check_when(L, when, when_node, who, error); rc != 0) return rc;

    const char* for_ticks = fields[2];
    const char* cooldown  = fields[3];
    const char* severity  = fields[4];
    const char* action    = fields[5];
    const char* message   = fields[6];

    // Whole non-negative integers; "5s", "-1" or "abc" is an error, not
    // a silently dropped field.
    long for_ticks_v = 0, cooldown_v = 0;
    if (for_ticks != nullptr && !parse_int_full(for_ticks, 0, INT_MAX, &for_ticks_v)) {
        *error = who + ": for_ticks '" + for_ticks + "' is not a whole number of ticks";
        return -EINVAL;
    }
    if (cooldown != nullptr && !parse_int_full(cooldown, 0, INT_MAX, &cooldown_v)) {
        *error = who + ": cooldown '" + cooldown + "' is not a whole number of ticks";
        return -EINVAL;
    }

    // As watch() takes them: any case, and anything else an error. A
    // `severity: Critical` became a warning without a word, and an
    // `action: escalate` an alert.
    AlertSeverity sev = AlertSeverity::Warning;
    if (severity != nullptr && !parse_severity(severity, &sev)) {
        *error = who + ": severity '" + severity + "' is not info, warning or critical";
        return -EINVAL;
    }
    const char* sev_name = severity_name(sev);
    if (action == nullptr) action = "alert";
    const bool is_log      = std::strcmp(action, "log") == 0;
    const bool is_escalate = std::strcmp(action, "escalate") == 0;
    if (!is_log && !is_escalate && std::strcmp(action, "alert") != 0) {
        *error = who + ": action '" + action + "' is not alert, log or escalate";
        return -EINVAL;
    }
    if (message == nullptr) message = name;

    const std::string nameL = lua_escape(name);
    const std::string msgL  = lua_escape(message);

    // Build the watch() call. `action` becomes a Lua closure that fires
    // alert(name, severity, message) or print(...); `escalate` is the
    // spec's `{ alert, escalate }`. The `when` text is spliced verbatim
    // (checked above), on a line of its own: a `--` comment at its end
    // would take the `end` with it.
    *out += "watch(\"";
    *out += nameL;
    *out += "\", {\n  when = function() return ";
    *out += when;
    *out += "\n  end,\n  action = ";
    if (is_escalate) *out += "{ ";
    *out += "function() ";
    if (is_log) {
        *out += "print(\"[budyk] ";
        *out += msgL;
        *out += "\")";
    } else {
        *out += "alert(\"";
        *out += nameL;
        *out += "\", \"";
        *out += sev_name;
        *out += "\", \"";
        *out += msgL;
        *out += "\")";
    }
    *out += " end";
    if (is_escalate) *out += ", \"escalate\" }";
    *out += ",\n";

    if (for_ticks_v > 0) {
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "  for_ticks = %ld,\n", for_ticks_v);
        *out += tmp;
    }
    if (cooldown != nullptr) {
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "  cooldown = %ld,\n", cooldown_v);
        *out += tmp;
    }
    *out += "})\n";
    return 0;
}

int walk_sequence(yaml_document_t* doc, const yaml_node_t* seq, lua_State* L,
                  std::string* out, std::string* error) {
    if (seq == nullptr || seq->type != YAML_SEQUENCE_NODE) {
        *error = "'rules:' is not a list";
        return -EINVAL;
    }
    size_t index = 0;
    for (auto* it = seq->data.sequence.items.start;
         it     != seq->data.sequence.items.top; ++it) {
        const yaml_node_t* item = yaml_document_get_node(doc, *it);
        const int r = transpile_rule(doc, item, ++index, L, out, error);
        if (r != 0) return r;
    }
    return 0;
}

// The YAML problem, with its line, or what a second document is.
std::string yaml_problem(const yaml_parser_t* parser) {
    return std::string("YAML: ") + (parser->problem != nullptr ? parser->problem : "cannot parse") +
           " at line " + std::to_string(parser->problem_mark.line + 1);
}

} // namespace

int yaml_rules_to_lua(const char* yaml_text, std::string* out, std::string* error) {
    std::string unused;
    if (error == nullptr) error = &unused;
    error->clear();
    if (yaml_text == nullptr || out == nullptr) {
        *error = "no text";
        return -EINVAL;
    }

    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_string(&parser,
        reinterpret_cast<const unsigned char*>(yaml_text),
        std::strlen(yaml_text));

    yaml_document_t doc;
    if (!yaml_parser_load(&parser, &doc)) {
        *error = yaml_problem(&parser);
        yaml_parser_delete(&parser);
        return -EINVAL;
    }

    // A scratch Lua state for the `when` checks: no libraries, nothing
    // of the engine. Without one (no memory) the load's check stands.
    lua_State* L = luaL_newstate();

    int rc = 0;
    const yaml_node_t* root = yaml_document_get_root_node(&doc);
    if (root == nullptr) {
        // Empty document → no rules. Not an error.
    } else if (root->type == YAML_SEQUENCE_NODE) {
        rc = walk_sequence(&doc, root, L, out, error);
    } else if (root->type == YAML_MAPPING_NODE) {
        const yaml_node_t* rules = yaml_find_key(&doc, root, "rules");
        if (rules == nullptr) {
            *error = "no 'rules:' list in the document";
            rc = -EINVAL;
        } else {
            rc = walk_sequence(&doc, rules, L, out, error);
        }
    } else {
        *error = "the document is neither a list of rules nor a mapping with 'rules:'";
        rc = -EINVAL;
    }
    if (L != nullptr) lua_close(L);
    yaml_document_delete(&doc);

    // Only the first document was read: rules after a `---` were dropped,
    // and a syntax error there never reported.
    if (rc == 0) {
        yaml_document_t more;
        if (!yaml_parser_load(&parser, &more)) {
            *error = yaml_problem(&parser);
            rc = -EINVAL;
        } else {
            const yaml_node_t* second = yaml_document_get_root_node(&more);
            if (second != nullptr && !is_null(second)) {
                *error = "a second YAML document (after ---): the file must hold one";
                rc = -EINVAL;
            }
            yaml_document_delete(&more);
        }
    }
    yaml_parser_delete(&parser);
    return rc;
}

int yaml_rules_to_lua(const std::string& yaml_text, std::string* out, std::string* error) {
    // The text goes on as a C string: a NUL byte ended it, and every
    // rule after the byte was dropped in silence.
    const size_t nul = yaml_text.find('\0');
    if (nul != std::string::npos) {
        if (error != nullptr) {
            *error = "a NUL byte at offset " + std::to_string(nul) + ": not a text file";
        }
        return -EINVAL;
    }
    return yaml_rules_to_lua(yaml_text.c_str(), out, error);
}

int yaml_rules_to_lua_file(const char* path, std::string* out, std::string* error) {
    std::string unused;
    if (error == nullptr) error = &unused;
    if (path == nullptr || out == nullptr) {
        *error = "no path";
        return -EINVAL;
    }
    std::string text, why;
    const int rc = read_text_file(path, &text, &why);
    if (rc != 0) {
        *error = "cannot read: " + why;
        return rc;
    }
    return yaml_rules_to_lua(text, out, error);
}

} // namespace budyk
