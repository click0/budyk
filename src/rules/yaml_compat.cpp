// SPDX-License-Identifier: BSD-3-Clause
#include "rules/yaml_compat.h"
#include "core/parse_int.h"
#include "rules/alert.h"
#include "util/yaml_dom.h"

extern "C" {
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

// The rule's place in an error message: its name once it has one, else
// its position in the list (from 1).
int transpile_rule(yaml_document_t* doc, const yaml_node_t* item, size_t index,
                   std::string* out, std::string* error) {
    const std::string at = "rule " + std::to_string(index);
    if (item == nullptr || item->type != YAML_MAPPING_NODE) {
        *error = at + " is not a mapping (name:, when:, ...)";
        return -EINVAL;
    }

    const char* name = yaml_scalar(yaml_find_key(doc, item, "name"));
    const char* when = yaml_scalar(yaml_find_key(doc, item, "when"));
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

    const char* for_ticks = yaml_scalar(yaml_find_key(doc, item, "for_ticks"));
    const char* cooldown  = yaml_scalar(yaml_find_key(doc, item, "cooldown"));
    const char* severity  = yaml_scalar(yaml_find_key(doc, item, "severity"));
    const char* action    = yaml_scalar(yaml_find_key(doc, item, "action"));
    const char* message   = yaml_scalar(yaml_find_key(doc, item, "message"));

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
    // spec's `{ alert, escalate }`. The `when` text is spliced verbatim —
    // Lua's parser surfaces any syntax errors at load time, same as for
    // hand-written rules.lua.
    *out += "watch(\"";
    *out += nameL;
    *out += "\", {\n  when = function() return ";
    *out += when;
    *out += " end,\n  action = ";
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

int walk_sequence(yaml_document_t* doc, const yaml_node_t* seq,
                  std::string* out, std::string* error) {
    if (seq == nullptr || seq->type != YAML_SEQUENCE_NODE) {
        *error = "'rules:' is not a list";
        return -EINVAL;
    }
    size_t index = 0;
    for (auto* it = seq->data.sequence.items.start;
         it     != seq->data.sequence.items.top; ++it) {
        const yaml_node_t* item = yaml_document_get_node(doc, *it);
        const int r = transpile_rule(doc, item, ++index, out, error);
        if (r != 0) return r;
    }
    return 0;
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
        *error = std::string("YAML: ") + (parser.problem != nullptr ? parser.problem : "cannot parse") +
                 " at line " + std::to_string(parser.problem_mark.line + 1);
        yaml_parser_delete(&parser);
        return -EINVAL;
    }

    int rc = 0;
    const yaml_node_t* root = yaml_document_get_root_node(&doc);
    if (root == nullptr) {
        // Empty document → no rules. Not an error.
    } else if (root->type == YAML_SEQUENCE_NODE) {
        rc = walk_sequence(&doc, root, out, error);
    } else if (root->type == YAML_MAPPING_NODE) {
        const yaml_node_t* rules = yaml_find_key(&doc, root, "rules");
        if (rules == nullptr) {
            *error = "no 'rules:' list in the document";
            rc = -EINVAL;
        } else {
            rc = walk_sequence(&doc, rules, out, error);
        }
    } else {
        *error = "the document is neither a list of rules nor a mapping with 'rules:'";
        rc = -EINVAL;
    }

    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    return rc;
}

int yaml_rules_to_lua_file(const char* path, std::string* out, std::string* error) {
    std::string unused;
    if (error == nullptr) error = &unused;
    if (path == nullptr || out == nullptr) {
        *error = "no path";
        return -EINVAL;
    }

    auto read_failed = [&](int err) {
        *error = std::string("cannot read: ") + std::strerror(err);
        return -err;
    };
    std::FILE* f = std::fopen(path, "rbe");
    if (f == nullptr) return read_failed(errno);
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return read_failed(EIO); }
    const long sz = std::ftell(f);
    if (sz < 0)                          { std::fclose(f); return read_failed(EIO); }
    std::fseek(f, 0, SEEK_SET);

    std::string text(static_cast<size_t>(sz), '\0');
    if (sz > 0 && std::fread(&text[0], 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        std::fclose(f);
        return read_failed(EIO);
    }
    std::fclose(f);
    return yaml_rules_to_lua(text.c_str(), out, error);
}

} // namespace budyk
