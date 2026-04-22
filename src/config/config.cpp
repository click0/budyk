// SPDX-License-Identifier: BSD-3-Clause
#include "config/config.h"

extern "C" {
#include <yaml.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace budyk {

namespace {

// --- YAML DOM helpers -------------------------------------------------------

const yaml_node_t* find_key(yaml_document_t* doc, const yaml_node_t* map, const char* key) {
    if (map == nullptr || map->type != YAML_MAPPING_NODE) return nullptr;
    for (auto* pair = map->data.mapping.pairs.start;
         pair     != map->data.mapping.pairs.top; ++pair) {
        yaml_node_t* k = yaml_document_get_node(doc, pair->key);
        if (k == nullptr || k->type != YAML_SCALAR_NODE) continue;
        if (std::strcmp(reinterpret_cast<const char*>(k->data.scalar.value), key) == 0) {
            return yaml_document_get_node(doc, pair->value);
        }
    }
    return nullptr;
}

const char* scalar_str(const yaml_node_t* n) {
    if (n == nullptr || n->type != YAML_SCALAR_NODE) return nullptr;
    return reinterpret_cast<const char*>(n->data.scalar.value);
}

void apply_str(yaml_document_t* d, const yaml_node_t* m, const char* key,
               char* dst, size_t dst_cap) {
    const char* v = scalar_str(find_key(d, m, key));
    if (v == nullptr) return;
    std::strncpy(dst, v, dst_cap - 1);
    dst[dst_cap - 1] = '\0';
}

void apply_int(yaml_document_t* d, const yaml_node_t* m, const char* key, int* dst) {
    const char* v = scalar_str(find_key(d, m, key));
    if (v == nullptr) return;
    char* end = nullptr;
    long n = std::strtol(v, &end, 10);
    if (end != v) *dst = static_cast<int>(n);
}

void apply_double(yaml_document_t* d, const yaml_node_t* m, const char* key, double* dst) {
    const char* v = scalar_str(find_key(d, m, key));
    if (v == nullptr) return;
    char* end = nullptr;
    double x = std::strtod(v, &end);
    if (end != v) *dst = x;
}

void apply_bool(yaml_document_t* d, const yaml_node_t* m, const char* key, bool* dst) {
    const char* v = scalar_str(find_key(d, m, key));
    if (v == nullptr) return;
    if (std::strcmp(v, "true") == 0 || std::strcmp(v, "yes") == 0 || std::strcmp(v, "on") == 0)
        *dst = true;
    else if (std::strcmp(v, "false") == 0 || std::strcmp(v, "no") == 0 || std::strcmp(v, "off") == 0)
        *dst = false;
}

// Walks a YAML sequence of scalars under `key` into dst. A non-sequence
// node (or missing key) leaves dst untouched. Empty sequences clear dst
// — useful for admins explicitly resetting an inherited allowlist.
void apply_str_list(yaml_document_t* d, const yaml_node_t* m, const char* key,
                    std::vector<std::string>* dst) {
    const yaml_node_t* seq = find_key(d, m, key);
    if (seq == nullptr || seq->type != YAML_SEQUENCE_NODE) return;
    dst->clear();
    for (auto* item = seq->data.sequence.items.start;
         item     != seq->data.sequence.items.top; ++item) {
        const yaml_node_t* n = yaml_document_get_node(d, *item);
        const char* s = scalar_str(n);
        if (s != nullptr) dst->emplace_back(s);
    }
}

// --- Section walkers --------------------------------------------------------

void apply_collection(yaml_document_t* d, const yaml_node_t* col, Config* out) {
    if (col == nullptr) return;

    if (auto* l1 = find_key(d, col, "l1"))
        apply_int(d, l1, "interval", &out->scheduler.l1_interval_sec);

    if (auto* l2 = find_key(d, col, "l2")) {
        apply_int (d, l2, "interval",   &out->scheduler.l2_interval_sec);
        apply_bool(d, l2, "always_on",  &out->scheduler.l2_always_on);
        apply_int (d, l2, "hysteresis", &out->scheduler.hysteresis_sec);
        if (auto* e = find_key(d, l2, "escalation_thresholds")) {
            apply_double(d, e, "load_1m",           &out->scheduler.escalation_load_1m);
            apply_double(d, e, "cpu_percent",       &out->scheduler.escalation_cpu_percent);
            apply_double(d, e, "swap_used_percent", &out->scheduler.escalation_swap_percent);
        }
    }
    if (auto* l3 = find_key(d, col, "l3")) {
        apply_int(d, l3, "interval",     &out->scheduler.l3_interval_sec);
        apply_int(d, l3, "grace_period", &out->scheduler.grace_period_sec);
    }
    if (auto* hb = find_key(d, col, "hot_buffer")) {
        apply_int(d, hb, "capacity",    &out->hot_buffer_capacity);
        apply_int(d, hb, "warm_grace",  &out->hot_buffer_warm_grace);
    }
}

int parse_document(yaml_parser_t* parser, Config* out) {
    yaml_document_t doc;
    if (!yaml_parser_load(parser, &doc)) return -3;

    const yaml_node_t* root = yaml_document_get_root_node(&doc);
    if (root == nullptr || root->type != YAML_MAPPING_NODE) {
        yaml_document_delete(&doc);
        return -4;
    }

    apply_str(&doc, root, "listen",   out->listen_addr, sizeof(out->listen_addr));
    apply_int(&doc, root, "port",    &out->listen_port);
    apply_str(&doc, root, "data_dir", out->data_dir,    sizeof(out->data_dir));

    apply_collection(&doc, find_key(&doc, root, "collection"), out);

    if (auto* storage = find_key(&doc, root, "storage")) {
        apply_int(&doc, storage, "tier1_max_mb", &out->tier1_max_mb);
        apply_int(&doc, storage, "tier2_max_mb", &out->tier2_max_mb);
        apply_int(&doc, storage, "tier3_max_mb", &out->tier3_max_mb);
    }

    if (auto* rules = find_key(&doc, root, "rules")) {
        apply_str (&doc, rules, "path",        out->rules_path, sizeof(out->rules_path));
        apply_bool(&doc, rules, "enable_exec", &out->rules_enable_exec);   // legacy flat key

        // Nested `rules.exec` block — preferred spelling. Its `enabled`
        // also maps to rules_enable_exec, so either syntax works:
        //   rules:
        //     enable_exec: true        # flat (legacy)
        //   rules:
        //     exec:
        //       enabled: true          # nested (preferred)
        //       allow:
        //         - /usr/local/bin/alerter
        //         - /usr/bin/systemctl
        if (auto* exec = find_key(&doc, rules, "exec")) {
            apply_bool    (&doc, exec, "enabled", &out->rules_enable_exec);
            apply_str_list(&doc, exec, "allow",   &out->rules_exec_allow);
        }
    }

    if (auto* web = find_key(&doc, root, "web")) {
        if (auto* auth = find_key(&doc, web, "auth")) {
            apply_bool(&doc, auth, "enabled", &out->auth_enabled);
            apply_str (&doc, auth, "password_hash",
                       out->password_hash, sizeof(out->password_hash));
        }
    }

    yaml_document_delete(&doc);
    return 0;
}

} // namespace

int config_load(const char* path, Config* out) {
    if (path == nullptr || out == nullptr) return -1;

    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return -2;

    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, f);
    int rc = parse_document(&parser, out);
    yaml_parser_delete(&parser);
    std::fclose(f);
    return rc;
}

int config_load_string(const char* yaml, Config* out) {
    if (yaml == nullptr || out == nullptr) return -1;

    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_string(&parser,
        reinterpret_cast<const unsigned char*>(yaml), std::strlen(yaml));
    int rc = parse_document(&parser, out);
    yaml_parser_delete(&parser);
    return rc;
}

} // namespace budyk
