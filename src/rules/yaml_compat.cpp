// SPDX-License-Identifier: BSD-3-Clause
#include "rules/yaml_compat.h"

extern "C" {
#include <yaml.h>
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace budyk {

namespace {

// --- libyaml DOM helpers (mirrors the pattern in src/config/config.cpp) ---

const char* scalar_str(const yaml_node_t* n) {
    if (n == nullptr || n->type != YAML_SCALAR_NODE) return nullptr;
    return reinterpret_cast<const char*>(n->data.scalar.value);
}

const yaml_node_t* find_key(yaml_document_t* doc,
                            const yaml_node_t* map, const char* key) {
    if (map == nullptr || map->type != YAML_MAPPING_NODE) return nullptr;
    for (auto* pair = map->data.mapping.pairs.start;
         pair     != map->data.mapping.pairs.top; ++pair) {
        const yaml_node_t* k = yaml_document_get_node(doc, pair->key);
        const char* ks = scalar_str(k);
        if (ks != nullptr && std::strcmp(ks, key) == 0) {
            return yaml_document_get_node(doc, pair->value);
        }
    }
    return nullptr;
}

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
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\%d", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

bool valid_severity(const char* s) {
    return std::strcmp(s, "info")     == 0 ||
           std::strcmp(s, "warning")  == 0 ||
           std::strcmp(s, "critical") == 0;
}

int transpile_rule(yaml_document_t* doc, const yaml_node_t* item,
                   std::string* out) {
    if (item == nullptr || item->type != YAML_MAPPING_NODE) return -EINVAL;

    const char* name = scalar_str(find_key(doc, item, "name"));
    const char* when = scalar_str(find_key(doc, item, "when"));
    if (name == nullptr || name[0] == '\0') return -EINVAL;
    if (when == nullptr || when[0] == '\0') return -EINVAL;

    // A newline in `when` would let the spliced text close `function() return …`
    // early and inject statements. Reject it — operators who really want
    // multi-line logic should drop to native Lua.
    for (const char* p = when; *p != '\0'; ++p) {
        if (*p == '\n' || *p == '\r') return -EINVAL;
    }

    const char* for_ticks = scalar_str(find_key(doc, item, "for_ticks"));
    const char* cooldown  = scalar_str(find_key(doc, item, "cooldown"));
    const char* severity  = scalar_str(find_key(doc, item, "severity"));
    const char* action    = scalar_str(find_key(doc, item, "action"));
    const char* message   = scalar_str(find_key(doc, item, "message"));

    if (severity == nullptr || !valid_severity(severity)) severity = "warning";
    if (action   == nullptr)                              action   = "alert";
    if (message  == nullptr)                              message  = name;

    const std::string nameL = lua_escape(name);
    const std::string msgL  = lua_escape(message);

    // Build the watch() call. `action` becomes a Lua closure that fires
    // alert(name, severity, message) or print(...). The `when` text is
    // spliced verbatim — Lua's parser surfaces any syntax errors at
    // load time, same as for hand-written rules.lua.
    *out += "watch(\"";
    *out += nameL;
    *out += "\", {\n  when = function() return ";
    *out += when;
    *out += " end,\n  action = function() ";
    if (std::strcmp(action, "log") == 0) {
        *out += "print(\"[budyk] ";
        *out += msgL;
        *out += "\")";
    } else {
        // alert (default)
        *out += "alert(\"";
        *out += nameL;
        *out += "\", \"";
        *out += severity;
        *out += "\", \"";
        *out += msgL;
        *out += "\")";
    }
    *out += " end,\n";

    if (for_ticks != nullptr) {
        const int v = std::atoi(for_ticks);
        if (v > 0) {
            char tmp[64];
            std::snprintf(tmp, sizeof(tmp), "  for_ticks = %d,\n", v);
            *out += tmp;
        }
    }
    if (cooldown != nullptr) {
        const int v = std::atoi(cooldown);
        if (v >= 0) {
            char tmp[64];
            std::snprintf(tmp, sizeof(tmp), "  cooldown = %d,\n", v);
            *out += tmp;
        }
    }
    *out += "})\n";
    return 0;
}

int walk_sequence(yaml_document_t* doc, const yaml_node_t* seq,
                  std::string* out) {
    if (seq == nullptr || seq->type != YAML_SEQUENCE_NODE) return -EINVAL;
    for (auto* it = seq->data.sequence.items.start;
         it     != seq->data.sequence.items.top; ++it) {
        const yaml_node_t* item = yaml_document_get_node(doc, *it);
        const int r = transpile_rule(doc, item, out);
        if (r != 0) return r;
    }
    return 0;
}

} // namespace

int yaml_rules_to_lua(const char* yaml_text, std::string* out) {
    if (yaml_text == nullptr || out == nullptr) return -EINVAL;

    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_string(&parser,
        reinterpret_cast<const unsigned char*>(yaml_text),
        std::strlen(yaml_text));

    yaml_document_t doc;
    if (!yaml_parser_load(&parser, &doc)) {
        yaml_parser_delete(&parser);
        return -EINVAL;
    }

    int rc = 0;
    const yaml_node_t* root = yaml_document_get_root_node(&doc);
    if (root == nullptr) {
        // Empty document → no rules. Not an error.
    } else if (root->type == YAML_SEQUENCE_NODE) {
        rc = walk_sequence(&doc, root, out);
    } else if (root->type == YAML_MAPPING_NODE) {
        const yaml_node_t* rules = find_key(&doc, root, "rules");
        if (rules == nullptr) {
            rc = -EINVAL;
        } else {
            rc = walk_sequence(&doc, rules, out);
        }
    } else {
        rc = -EINVAL;
    }

    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    return rc;
}

int yaml_rules_to_lua_file(const char* path, std::string* out) {
    if (path == nullptr || out == nullptr) return -EINVAL;

    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return -errno;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return -EIO; }
    const long sz = std::ftell(f);
    if (sz < 0)                          { std::fclose(f); return -EIO; }
    std::fseek(f, 0, SEEK_SET);

    std::string text(static_cast<size_t>(sz), '\0');
    if (sz > 0 && std::fread(&text[0], 1, sz, f) != static_cast<size_t>(sz)) {
        std::fclose(f);
        return -EIO;
    }
    std::fclose(f);
    return yaml_rules_to_lua(text.c_str(), out);
}

} // namespace budyk
