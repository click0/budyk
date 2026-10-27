// SPDX-License-Identifier: BSD-3-Clause
#include "config/config.h"
#include "util/yaml_dom.h"

extern "C" {
#include <yaml.h>
}

#include "core/parse_int.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

#include <strings.h>
#include <unistd.h>

namespace budyk {

namespace {

// YAML's null: a plain `~`, `null` or nothing at all (`allow:` with every
// item commented out). A key with no value is as if it were not there.
bool yaml_null(const yaml_node_t* n) {
    if (n == nullptr || n->type != YAML_SCALAR_NODE ||
        n->data.scalar.style != YAML_PLAIN_SCALAR_STYLE) {
        return false;
    }
    const char* v = reinterpret_cast<const char*>(n->data.scalar.value);
    return *v == '\0' || std::strcmp(v, "~") == 0 || ::strcasecmp(v, "null") == 0;
}

// The value under `key`, or nullptr when there is none. A list or a
// mapping where a value belongs is reported under the setting's full
// name (`path`), and the setting keeps its default.
const char* scalar_at(yaml_document_t* d, const yaml_node_t* m, const char* key,
                      const char* path) {
    const yaml_node_t* n = yaml_find_key(d, m, key);
    if (n == nullptr || yaml_null(n)) return nullptr;
    if (const char* v = yaml_scalar(n)) return v;
    std::fprintf(stderr, "budyk config: %s: a %s where a value belongs; ignored\n", path,
                 n->type == YAML_SEQUENCE_NODE ? "list" : "mapping");
    return nullptr;
}

void apply_str(yaml_document_t* d, const yaml_node_t* m, const char* key,
               char* dst, size_t dst_cap, const char* path) {
    const char* v = scalar_at(d, m, key, path);
    if (v == nullptr) return;
    std::strncpy(dst, v, dst_cap - 1);
    dst[dst_cap - 1] = '\0';
}

// The numbers are read whole: strtol() / strtod() stopped at the first
// character that was not a digit and kept what they had, so "5m" was 5,
// "1.5h" 1, "4294967596" 300 and "nan" a threshold that never fires. A
// value that does not read is reported, under its full name (`path`),
// and the default is kept.

// Seconds from a duration: a number with an optional unit, ms, s, m (or
// min), h or d: "300", "300s" (the spec's form), "5m", "1.5h", "500ms".
bool parse_duration_full(const char* v, double* seconds) {
    if (v == nullptr || *v == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const double x = std::strtod(v, &end);
    if (end == v || errno == ERANGE || !std::isfinite(x)) return false;
    double unit = 0;
    if      (*end == '\0' || std::strcmp(end, "s") == 0)   unit = 1.0;
    else if (std::strcmp(end, "ms") == 0)                   unit = 0.001;
    else if (std::strcmp(end, "m") == 0 || std::strcmp(end, "min") == 0) unit = 60.0;
    else if (std::strcmp(end, "h") == 0)                    unit = 3600.0;
    else if (std::strcmp(end, "d") == 0)                    unit = 86400.0;
    else return false;
    *seconds = x * unit;
    return true;
}

void apply_int(yaml_document_t* d, const yaml_node_t* m, const char* key, int* dst,
               const char* path) {
    const char* v = scalar_at(d, m, key, path);
    if (v == nullptr) return;
    long n = 0;
    if (!parse_int_full(v, INT_MIN, INT_MAX, &n)) {
        std::fprintf(stderr, "budyk config: %s: '%s' is not a whole number; using %d\n",
                     path, v, *dst);
        return;
    }
    *dst = static_cast<int>(n);
}

// Whole seconds, with a unit or without (parse_duration_full).
void apply_seconds(yaml_document_t* d, const yaml_node_t* m, const char* key, int* dst,
                   const char* path) {
    const char* v = scalar_at(d, m, key, path);
    if (v == nullptr) return;
    double x = 0;
    if (!parse_duration_full(v, &x) || x != std::floor(x) || x < INT_MIN || x > INT_MAX) {
        std::fprintf(stderr, "budyk config: %s: '%s' is not whole seconds (300, 300s, 5m, 1h); "
                             "using %d\n", path, v, *dst);
        return;
    }
    *dst = static_cast<int>(x);
}

void apply_double(yaml_document_t* d, const yaml_node_t* m, const char* key, double* dst,
                  const char* path) {
    const char* v = scalar_at(d, m, key, path);
    if (v == nullptr) return;
    errno = 0;
    char* end = nullptr;
    const double x = std::strtod(v, &end);
    if (end == v || *end != '\0' || errno == ERANGE || !std::isfinite(x)) {
        std::fprintf(stderr, "budyk config: %s: '%s' is not a number; using %g\n", path, v, *dst);
        return;
    }
    *dst = x;
}

// YAML's booleans in any case: true / yes / on, false / no / off (`True`
// is what Python and Ansible write). Anything else is reported and the
// default kept: `web.auth.enabled: True` used to leave the dashboard
// without a password, without a word.
void apply_bool(yaml_document_t* d, const yaml_node_t* m, const char* key, bool* dst,
                const char* path) {
    const char* v = scalar_at(d, m, key, path);
    if (v == nullptr) return;
    if (::strcasecmp(v, "true") == 0 || ::strcasecmp(v, "yes") == 0 ||
        ::strcasecmp(v, "on") == 0) {
        *dst = true;
    } else if (::strcasecmp(v, "false") == 0 || ::strcasecmp(v, "no") == 0 ||
               ::strcasecmp(v, "off") == 0) {
        *dst = false;
    } else {
        std::fprintf(stderr, "budyk config: %s: '%s' is not true or false; using %s\n",
                     path, v, *dst ? "true" : "false");
    }
}

// A list of strings under `key` into dst, or one string as a list of one
// (`allow: /usr/bin/systemctl` used to leave the exec allowlist empty,
// and an empty one lets exec() run any absolute path). An empty list
// clears dst: an admin's explicit reset. An item that is not a string is
// reported and left out; a mapping is reported and dst kept.
void apply_str_list(yaml_document_t* d, const yaml_node_t* m, const char* key,
                    std::vector<std::string>* dst, const char* path) {
    const yaml_node_t* seq = yaml_find_key(d, m, key);
    if (seq == nullptr || yaml_null(seq)) return;
    if (const char* one = yaml_scalar(seq)) {
        dst->assign(1, one);
        return;
    }
    if (seq->type != YAML_SEQUENCE_NODE) {
        std::fprintf(stderr, "budyk config: %s: a mapping where a list belongs; ignored\n", path);
        return;
    }
    dst->clear();
    size_t index = 0;
    for (auto* item = seq->data.sequence.items.start;
         item     != seq->data.sequence.items.top; ++item, ++index) {
        const yaml_node_t* n = yaml_document_get_node(d, *item);
        const char* s = yaml_scalar(n);
        if (s != nullptr) {
            dst->emplace_back(s);
        } else {
            std::fprintf(stderr, "budyk config: %s[%zu]: not a string; left out\n", path, index);
        }
    }
}

// Walks a YAML sequence of mappings under `key` and converts each item
// into an AlertChannelConfig via the {name,type,url,topic,token,from}
// scalar fields. A missing or non-sequence node leaves dst untouched.
void apply_alert_channels(yaml_document_t* d, const yaml_node_t* m,
                          std::vector<Config::AlertChannelConfig>* dst) {
    const yaml_node_t* seq = yaml_find_key(d, m, "channels");
    if (seq == nullptr || seq->type != YAML_SEQUENCE_NODE) return;
    dst->clear();
    size_t index = 0;
    for (auto* item = seq->data.sequence.items.start;
         item     != seq->data.sequence.items.top; ++item, ++index) {
        const yaml_node_t* n = yaml_document_get_node(d, *item);
        const std::string where = "alerts.channels[" + std::to_string(index) + "]";
        if (n == nullptr || n->type != YAML_MAPPING_NODE) {
            std::fprintf(stderr, "budyk config: %s: not a mapping; left out\n", where.c_str());
            continue;
        }
        Config::AlertChannelConfig ch;
        const struct { const char* key; std::string* dst; } fields[] = {
            {"name", &ch.name}, {"type", &ch.type}, {"url", &ch.url},
            {"topic", &ch.topic}, {"token", &ch.token}, {"from", &ch.from},
        };
        for (const auto& f : fields) {
            const std::string path = where + "." + f.key;
            if (const char* v = scalar_at(d, n, f.key, path.c_str())) *f.dst = v;
        }
        // A channel with no type has nowhere to go: left out, and said so.
        if (ch.type.empty()) {
            std::fprintf(stderr,
                "budyk config: %s (%s): no type (ntfy, discord, telegram, smtp, twilio); "
                "left out\n", where.c_str(), ch.name.empty() ? "?" : ch.name.c_str());
            continue;
        }
        dst->emplace_back(std::move(ch));
    }
}

// The keys the loader reads, section by section. A key it does not know
// is reported, and so is one given twice (the first is the one read): a
// typo such as `web.auth.enable: true` left the dashboard without a
// password, without a word.
struct KeySpec {
    const char*    name;
    const KeySpec* keys  = nullptr;   // a section's own keys; nullptr for a value
    bool           items = false;     // a list whose items are sections of `keys`
};

const KeySpec kL1Keys[]         = {{"interval"}, {"metrics"}, {nullptr}};
const KeySpec kThresholdKeys[]  = {{"load_1m"}, {"cpu_percent"}, {"swap_used_percent"},
                                   {nullptr}};
const KeySpec kL2Keys[]         = {{"interval"}, {"always_on"}, {"hysteresis"},
                                   {"escalate_after"},
                                   {"escalation_thresholds", kThresholdKeys},
                                   {"metrics"}, {nullptr}};
const KeySpec kL3Keys[]         = {{"interval"}, {"grace_period"}, {"metrics"}, {nullptr}};
const KeySpec kLevelKeys[]      = {{"name"}, {"interval"}, {"priority"}, {"when"}, {"hold"},
                                   {"storage_mb"}, {"metrics"}, {nullptr}};
const KeySpec kHotBufferKeys[]  = {{"capacity"}, {"warm_grace"}, {nullptr}};
const KeySpec kCollectionKeys[] = {{"mode"}, {"l1", kL1Keys}, {"l2", kL2Keys},
                                   {"l3", kL3Keys}, {"levels", kLevelKeys, true},
                                   {"hot_buffer", kHotBufferKeys}, {nullptr}};
const KeySpec kStorageKeys[]    = {{"tier1_max_mb"}, {"tier2_max_mb"}, {"tier3_max_mb"},
                                   {nullptr}};
const KeySpec kGateKeys[]       = {{"enabled"}, {"allow"}, {nullptr}};
const KeySpec kLimitKeys[]      = {{"instructions"}, {"memory_mb"}, {nullptr}};
const KeySpec kRulesKeys[]      = {{"path"}, {"enable_exec"}, {"persist_state"},
                                   {"state_path"}, {"exec", kGateKeys},
                                   {"freeze", kGateKeys}, {"limits", kLimitKeys},
                                   {nullptr}};
const KeySpec kChannelKeys[]    = {{"name"}, {"type"}, {"url"}, {"topic"}, {"token"},
                                   {"from"}, {nullptr}};
const KeySpec kAlertsKeys[]     = {{"channels", kChannelKeys, true}, {nullptr}};
const KeySpec kFileWatchKeys[]  = {{"enabled"}, {"paths"}, {nullptr}};
const KeySpec kSecurityKeys[]   = {{"file_watch", kFileWatchKeys}, {nullptr}};
const KeySpec kAuthKeys[]       = {{"enabled"}, {"password_hash"}, {"persist_sessions"},
                                   {"max_login_failures"}, {"login_window"}, {nullptr}};
const KeySpec kWebKeys[]        = {{"allowed_hosts"}, {"auth", kAuthKeys}, {nullptr}};
const KeySpec kRootKeys[]       = {{"listen"}, {"port"}, {"data_dir"},
                                   {"collection", kCollectionKeys},
                                   {"storage", kStorageKeys}, {"rules", kRulesKeys},
                                   {"alerts", kAlertsKeys}, {"security", kSecurityKeys},
                                   {"web", kWebKeys}, {nullptr}};

void check_keys(yaml_document_t* d, const yaml_node_t* m, const std::string& where,
                const KeySpec* spec) {
    const auto full = [&](const char* key) {
        return where.empty() ? std::string(key) : where + "." + key;
    };
    for (auto* pair = m->data.mapping.pairs.start; pair != m->data.mapping.pairs.top; ++pair) {
        const char* key = yaml_scalar(yaml_document_get_node(d, pair->key));
        if (key == nullptr) {
            std::fprintf(stderr, "budyk config: %s: a key that is not a name; ignored\n",
                         where.empty() ? "(top level)" : where.c_str());
            continue;
        }
        const KeySpec* k = spec;
        while (k->name != nullptr && std::strcmp(k->name, key) != 0) ++k;
        if (k->name == nullptr) {
            std::fprintf(stderr, "budyk config: %s: unknown key; ignored\n", full(key).c_str());
            continue;
        }
        for (auto* prev = m->data.mapping.pairs.start; prev != pair; ++prev) {
            const char* pk = yaml_scalar(yaml_document_get_node(d, prev->key));
            if (pk != nullptr && std::strcmp(pk, key) == 0) {
                std::fprintf(stderr, "budyk config: %s: given twice; the first is read\n",
                             full(key).c_str());
                break;
            }
        }
        const yaml_node_t* v = yaml_document_get_node(d, pair->value);
        if (k->keys == nullptr || v == nullptr || yaml_null(v)) continue;
        if (k->items) {
            if (v->type != YAML_SEQUENCE_NODE) {
                std::fprintf(stderr, "budyk config: %s: not a list; ignored\n", full(key).c_str());
                continue;
            }
            size_t index = 0;
            for (auto* item = v->data.sequence.items.start;
                 item     != v->data.sequence.items.top; ++item, ++index) {
                const yaml_node_t* n = yaml_document_get_node(d, *item);
                if (n != nullptr && n->type == YAML_MAPPING_NODE) {   // others: reported where read
                    check_keys(d, n, full(key) + "[" + std::to_string(index) + "]", k->keys);
                }
            }
        } else if (v->type == YAML_MAPPING_NODE) {
            check_keys(d, v, full(key), k->keys);
        } else {
            std::fprintf(stderr, "budyk config: %s: not a mapping of settings; ignored\n",
                         full(key).c_str());
        }
    }
}

// Whole-string numeric parses: "30s" or "" is not a number.
bool parse_long_full(const char* v, long* out) {
    if (v == nullptr || *v == '\0') return false;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v || *end != '\0') return false;
    *out = n;
    return true;
}

bool valid_level_name(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// <level>.metrics: a set name or a list of names (parse_metric_set). A
// bad one is logged and the level keeps its default.
void apply_metrics(yaml_document_t* d, const yaml_node_t* m, const char* where,
                   MetricSet* dst) {
    const yaml_node_t* n = yaml_find_key(d, m, "metrics");
    if (n == nullptr) return;
    std::vector<std::string> names;
    if (const char* v = yaml_scalar(n)) {
        names.emplace_back(v);
    } else if (n->type == YAML_SEQUENCE_NODE) {
        for (auto* item = n->data.sequence.items.start;
             item     != n->data.sequence.items.top; ++item) {
            const char* name = yaml_scalar(yaml_document_get_node(d, *item));
            names.emplace_back(name != nullptr ? name : "(not a name)");
        }
    } else {
        names.emplace_back("(not a name)");
    }
    std::string error;
    if (parse_metric_set(names, dst, &error) != 0) {
        std::fprintf(stderr,
            "budyk config: %s.metrics: %s (a set: minimal, standard, full; or a list "
            "of groups such as cpu, disk, net.interfaces); using the default\n",
            where, error.c_str());
    }
}

// collection.levels: a sequence of user-defined levels. Each needs name,
// interval (seconds, fractions allowed, 0.1 .. 86400) and priority; when,
// hold (seconds >= 0) and storage_mb (>= 1) are optional. A level that
// fails validation is logged and skipped — the rest still load — and
// accepted levels get ids kFirstCustomLevel, kFirstCustomLevel + 1, ...
void apply_custom_levels(yaml_document_t* d, const yaml_node_t* col,
                         std::vector<CustomLevel>* dst) {
    const yaml_node_t* seq = yaml_find_key(d, col, "levels");
    if (seq == nullptr || seq->type != YAML_SEQUENCE_NODE) return;
    dst->clear();
    size_t index = 0;
    for (auto* item = seq->data.sequence.items.start;
         item     != seq->data.sequence.items.top; ++item, ++index) {
        const yaml_node_t* n = yaml_document_get_node(d, *item);
        const char* nm = n != nullptr && n->type == YAML_MAPPING_NODE
                       ? yaml_scalar(yaml_find_key(d, n, "name")) : nullptr;
        auto reject = [&](const char* why) {
            std::fprintf(stderr,
                "budyk config: collection.levels[%zu] (%s): %s; level ignored\n",
                index, nm != nullptr ? nm : "?", why);
        };
        if (n == nullptr || n->type != YAML_MAPPING_NODE) { reject("not a mapping"); continue; }

        CustomLevel lv;
        if (nm == nullptr || !valid_level_name(nm)) {
            reject("'name' is required: 1-32 letters, digits, '_' or '-'");
            continue;
        }
        lv.name = nm;
        if (::strcasecmp(nm, "L1") == 0 || ::strcasecmp(nm, "L2") == 0 ||
            ::strcasecmp(nm, "L3") == 0) {
            reject("L1, L2 and L3 are built-in names");
            continue;
        }
        bool dup = false;
        for (const auto& other : *dst) dup = dup || ::strcasecmp(other.name.c_str(), nm) == 0;
        if (dup) { reject("duplicate name"); continue; }

        double interval_s = 0;
        if (!parse_duration_full(yaml_scalar(yaml_find_key(d, n, "interval")), &interval_s) ||
            interval_s < 0.1 || interval_s > 86400) {
            reject("'interval' is required: seconds from 0.1 to 86400 (0.5, 500ms, 30s, 5m)");
            continue;
        }
        lv.interval_ms = static_cast<int>(std::lround(interval_s * 1000.0));

        long prio = 0;
        if (!parse_long_full(yaml_scalar(yaml_find_key(d, n, "priority")), &prio) ||
            prio < -1000 || prio > 1000) {
            reject("'priority' is required: an integer (L1 = 0, L2 = 20, L3 = 30)");
            continue;
        }
        lv.priority = static_cast<int>(prio);

        if (const char* w = yaml_scalar(yaml_find_key(d, n, "when"))) lv.when = w;

        if (const yaml_node_t* h = yaml_find_key(d, n, "hold")) {
            double hold = 0;
            if (!parse_duration_full(yaml_scalar(h), &hold) || hold != std::floor(hold) ||
                hold < 0 || hold > 86400) {
                reject("'hold' must be whole seconds from 0 to 86400 (30, 30s, 5m)");
                continue;
            }
            lv.hold_sec = static_cast<int>(hold);
        }
        if (const yaml_node_t* s = yaml_find_key(d, n, "storage_mb")) {
            long mb = 0;
            if (!parse_long_full(yaml_scalar(s), &mb) || mb < 1 || mb > 100000) {
                reject("'storage_mb' must be an integer from 1 to 100000");
                continue;
            }
            lv.storage_mb = static_cast<int>(mb);
        }
        const std::string where = "collection.levels[" + std::to_string(index) + "]";
        apply_metrics(d, n, where.c_str(), &lv.metrics);

        if (dst->size() >= kMaxCustomLevels) {
            reject("too many levels (at most 16)");
            continue;
        }
        lv.id = static_cast<uint8_t>(kFirstCustomLevel + dst->size());
        dst->push_back(std::move(lv));
    }
}

// Replaces an out-of-range value with its default and says so. Values a
// daemon can't run with (a 0-record hot buffer divides by zero, a 0 s
// interval spins a core) must never reach it.
void clamp_int(int* v, int lo, int hi, int fallback, const char* key) {
    if (*v >= lo && *v <= hi) return;
    std::fprintf(stderr,
        "budyk config: %s = %d is out of range (%d..%d); using %d\n",
        key, *v, lo, hi, fallback);
    *v = fallback;
}

void clamp_numbers(Config* c) {
    const Config d;   // defaults
    clamp_int(&c->listen_port, 1, 65535, d.listen_port, "port");
    clamp_int(&c->scheduler.l1_interval_sec, 1, 86400, d.scheduler.l1_interval_sec, "collection.l1.interval");
    clamp_int(&c->scheduler.l2_interval_sec, 1, 86400, d.scheduler.l2_interval_sec, "collection.l2.interval");
    clamp_int(&c->scheduler.l3_interval_sec, 1, 86400, d.scheduler.l3_interval_sec, "collection.l3.interval");
    clamp_int(&c->scheduler.hysteresis_sec,   0, 86400, d.scheduler.hysteresis_sec,   "collection.l2.hysteresis");
    clamp_int(&c->scheduler.grace_period_sec, 0, 86400, d.scheduler.grace_period_sec, "collection.l3.grace_period");
    clamp_int(&c->scheduler.escalate_after_sec, 0, 86400, d.scheduler.escalate_after_sec, "collection.l2.escalate_after");
    clamp_int(&c->hot_buffer_capacity,   1, 100000, d.hot_buffer_capacity,   "collection.hot_buffer.capacity");
    clamp_int(&c->hot_buffer_warm_grace, 0, 86400,  d.hot_buffer_warm_grace, "collection.hot_buffer.warm_grace");
    clamp_int(&c->tier1_max_mb, 1, 100000, d.tier1_max_mb, "storage.tier1_max_mb");
    clamp_int(&c->tier2_max_mb, 1, 100000, d.tier2_max_mb, "storage.tier2_max_mb");
    clamp_int(&c->tier3_max_mb, 1, 100000, d.tier3_max_mb, "storage.tier3_max_mb");
    clamp_int(&c->rules_instruction_limit, 10000, 1000000000, d.rules_instruction_limit, "rules.limits.instructions");
    clamp_int(&c->rules_memory_mb,         1,     1024,       d.rules_memory_mb,         "rules.limits.memory_mb");
    clamp_int(&c->auth_max_login_failures, 1, 1000,  d.auth_max_login_failures, "web.auth.max_login_failures");
    clamp_int(&c->auth_login_window_sec,   1, 86400, d.auth_login_window_sec,   "web.auth.login_window");
}

// --- Section walkers --------------------------------------------------------

void apply_collection(yaml_document_t* d, const yaml_node_t* col, Config* out) {
    if (col == nullptr) return;

    if (const yaml_node_t* m = yaml_find_key(d, col, "mode")) {
        const char* v = yaml_scalar(m);
        if      (v != nullptr && std::strcmp(v, "adaptive")  == 0) out->scheduler.mode = CollectionMode::Adaptive;
        else if (v != nullptr && std::strcmp(v, "always")    == 0) out->scheduler.mode = CollectionMode::Always;
        else if (v != nullptr && std::strcmp(v, "on-demand") == 0) out->scheduler.mode = CollectionMode::OnDemand;
        else {
            std::fprintf(stderr,
                "budyk config: collection.mode: '%s' is not adaptive, always or on-demand; "
                "using adaptive\n", v != nullptr ? v : "(not a name)");
            out->scheduler.mode = CollectionMode::Adaptive;
        }
    }
    if (auto* l1 = yaml_find_key(d, col, "l1")) {
        apply_seconds(d, l1, "interval", &out->scheduler.l1_interval_sec, "collection.l1.interval");
        apply_metrics(d, l1, "collection.l1", &out->scheduler.l1_metrics);
    }

    if (auto* l2 = yaml_find_key(d, col, "l2")) {
        apply_seconds(d, l2, "interval",   &out->scheduler.l2_interval_sec, "collection.l2.interval");
        apply_bool   (d, l2, "always_on",  &out->scheduler.l2_always_on, "collection.l2.always_on");
        apply_seconds(d, l2, "hysteresis", &out->scheduler.hysteresis_sec, "collection.l2.hysteresis");
        apply_seconds(d, l2, "escalate_after", &out->scheduler.escalate_after_sec,
                      "collection.l2.escalate_after");
        apply_metrics(d, l2, "collection.l2", &out->scheduler.l2_metrics);
        if (auto* e = yaml_find_key(d, l2, "escalation_thresholds")) {
            apply_double(d, e, "load_1m",           &out->scheduler.escalation_load_1m,
                         "collection.l2.escalation_thresholds.load_1m");
            apply_double(d, e, "cpu_percent",       &out->scheduler.escalation_cpu_percent,
                         "collection.l2.escalation_thresholds.cpu_percent");
            apply_double(d, e, "swap_used_percent", &out->scheduler.escalation_swap_percent,
                         "collection.l2.escalation_thresholds.swap_used_percent");
        }
    }
    if (auto* l3 = yaml_find_key(d, col, "l3")) {
        apply_seconds(d, l3, "interval",     &out->scheduler.l3_interval_sec, "collection.l3.interval");
        apply_seconds(d, l3, "grace_period", &out->scheduler.grace_period_sec,
                      "collection.l3.grace_period");
        apply_metrics(d, l3, "collection.l3", &out->scheduler.l3_metrics);
    }
    apply_custom_levels(d, col, &out->scheduler.custom_levels);

    if (auto* hb = yaml_find_key(d, col, "hot_buffer")) {
        apply_int    (d, hb, "capacity",   &out->hot_buffer_capacity, "collection.hot_buffer.capacity");
        apply_seconds(d, hb, "warm_grace", &out->hot_buffer_warm_grace,
                      "collection.hot_buffer.warm_grace");
    }
}

// libyaml's reason, where (line and column from 1) and what it was in the
// middle of: "found unexpected end of stream at line 3, column 1 (while
// scanning a quoted scalar from line 2)". It prints nothing itself: the
// error used to be "YAML parse error" alone.
std::string yaml_problem(const yaml_parser_t* p) {
    std::string out = p->problem != nullptr ? p->problem : "not YAML";
    char at[96];
    std::snprintf(at, sizeof(at), " at line %zu, column %zu",
                  p->problem_mark.line + 1, p->problem_mark.column + 1);
    out += at;
    if (p->context != nullptr) {
        std::snprintf(at, sizeof(at), " from line %zu)", p->context_mark.line + 1);
        out += " (";
        out += p->context;
        out += at;
    }
    return out;
}

int parse_document(yaml_parser_t* parser, Config* out, std::string* error) {
    yaml_document_t doc;
    if (!yaml_parser_load(parser, &doc)) {
        if (error != nullptr) *error = yaml_problem(parser);
        return -3;
    }

    // An empty file, or one of comments only, holds no settings: the
    // defaults, as for keys left out (it was refused as a parse error).
    const yaml_node_t* root = yaml_document_get_root_node(&doc);
    if (root == nullptr || yaml_null(root)) {
        yaml_document_delete(&doc);
        clamp_numbers(out);
        return 0;
    }
    if (root->type != YAML_MAPPING_NODE) {
        if (error != nullptr) *error = "the top level is not a mapping of settings (key: value)";
        yaml_document_delete(&doc);
        return -4;
    }
    check_keys(&doc, root, "", kRootKeys);

    apply_str(&doc, root, "listen",   out->listen_addr, sizeof(out->listen_addr), "listen");
    apply_int(&doc, root, "port",    &out->listen_port, "port");
    apply_str(&doc, root, "data_dir", out->data_dir,    sizeof(out->data_dir), "data_dir");

    apply_collection(&doc, yaml_find_key(&doc, root, "collection"), out);

    if (auto* storage = yaml_find_key(&doc, root, "storage")) {
        apply_int(&doc, storage, "tier1_max_mb", &out->tier1_max_mb, "storage.tier1_max_mb");
        apply_int(&doc, storage, "tier2_max_mb", &out->tier2_max_mb, "storage.tier2_max_mb");
        apply_int(&doc, storage, "tier3_max_mb", &out->tier3_max_mb, "storage.tier3_max_mb");
    }

    if (auto* rules = yaml_find_key(&doc, root, "rules")) {
        // `path:` with nothing, `~` or "": no rules, as 0.8.3 read the
        // first. A null elsewhere keeps the default; here that loaded the
        // default rules file (the FreeBSD package installs one), in silence.
        const yaml_node_t* rp = yaml_find_key(&doc, rules, "path");
        if (rp != nullptr && yaml_null(rp)) {
            out->rules_path[0] = '\0';
        } else {
            apply_str(&doc, rules, "path", out->rules_path, sizeof(out->rules_path),
                      "rules.path");
        }
        apply_bool(&doc, rules, "enable_exec",   &out->rules_enable_exec,     // legacy flat key
                   "rules.enable_exec");
        apply_bool(&doc, rules, "persist_state", &out->rules_persist_state, "rules.persist_state");
        apply_str (&doc, rules, "state_path",    out->rules_state_path,
                   sizeof(out->rules_state_path), "rules.state_path");

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
        if (auto* exec = yaml_find_key(&doc, rules, "exec")) {
            apply_bool    (&doc, exec, "enabled", &out->rules_enable_exec, "rules.exec.enabled");
            apply_str_list(&doc, exec, "allow",   &out->rules_exec_allow, "rules.exec.allow");
        }

        // Nested `rules.freeze` block — gate + allowlist for
        // freeze() / unfreeze(). Format mirrors rules.exec.
        if (auto* fr = yaml_find_key(&doc, rules, "freeze")) {
            apply_bool    (&doc, fr, "enabled", &out->rules_enable_freeze, "rules.freeze.enabled");
            apply_str_list(&doc, fr, "allow",   &out->rules_freeze_allow, "rules.freeze.allow");
        }

        // Nested `rules.limits` block — how much a single call into Lua
        // may run and how much memory the engine may hold.
        if (auto* lim = yaml_find_key(&doc, rules, "limits")) {
            apply_int(&doc, lim, "instructions", &out->rules_instruction_limit,
                      "rules.limits.instructions");
            apply_int(&doc, lim, "memory_mb",    &out->rules_memory_mb, "rules.limits.memory_mb");
        }
    }

    if (auto* alerts = yaml_find_key(&doc, root, "alerts")) {
        apply_alert_channels(&doc, alerts, &out->alert_channels);
    }

    // security.file_watch — inotify/kqueue watcher that feeds the Lua
    // `files` global so tamper-detection rules can fire.
    if (auto* sec = yaml_find_key(&doc, root, "security")) {
        if (auto* fw = yaml_find_key(&doc, sec, "file_watch")) {
            apply_bool    (&doc, fw, "enabled", &out->file_watch_enabled,
                           "security.file_watch.enabled");
            apply_str_list(&doc, fw, "paths",   &out->file_watch_paths,
                           "security.file_watch.paths");
        }
    }

    if (auto* web = yaml_find_key(&doc, root, "web")) {
        if (auto* auth = yaml_find_key(&doc, web, "auth")) {
            apply_bool(&doc, auth, "enabled", &out->auth_enabled, "web.auth.enabled");
            apply_str (&doc, auth, "password_hash",
                       out->password_hash, sizeof(out->password_hash), "web.auth.password_hash");
            apply_bool(&doc, auth, "persist_sessions",
                       &out->auth_persist_sessions, "web.auth.persist_sessions");
            apply_int    (&doc, auth, "max_login_failures", &out->auth_max_login_failures,
                          "web.auth.max_login_failures");
            apply_seconds(&doc, auth, "login_window",       &out->auth_login_window_sec,
                          "web.auth.login_window");
        }
        apply_str_list(&doc, web, "allowed_hosts", &out->web_allowed_hosts, "web.allowed_hosts");
    }

    yaml_document_delete(&doc);
    clamp_numbers(out);
    return 0;
}

} // namespace

int config_load(const char* path, Config* out, std::string* error) {
    if (error != nullptr) error->clear();
    if (path == nullptr || out == nullptr) {
        if (error != nullptr) *error = "no path or no output";
        return -1;
    }

    FILE* f = std::fopen(path, "re");
    if (f == nullptr) {
        if (error != nullptr) *error = std::strerror(errno);
        return -2;
    }

    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, f);
    int rc = parse_document(&parser, out, error);
    yaml_parser_delete(&parser);
    std::fclose(f);
    return rc;
}

const char* default_config_path(bool (*exists)(const char* path)) {
    static const char* const kLocal = "/usr/local/etc/budyk/config.yaml";
#ifdef BUDYK_LINUX
    // The packages' config: `budyk fsck` and `suggest-rules` with no
    // --config looked only under /usr/local and gave up on a package's.
    static const char* const kEtc = "/etc/budyk/config.yaml";
    const auto there = [exists](const char* p) {
        return exists != nullptr ? exists(p) : ::access(p, F_OK) == 0;
    };
    if (!there(kLocal) && there(kEtc)) return kEtc;
#else
    (void)exists;
#endif
    return kLocal;
}

int config_load_string(const char* yaml, Config* out, std::string* error) {
    if (error != nullptr) error->clear();
    if (yaml == nullptr || out == nullptr) {
        if (error != nullptr) *error = "no text or no output";
        return -1;
    }

    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_string(&parser,
        reinterpret_cast<const unsigned char*>(yaml), std::strlen(yaml));
    int rc = parse_document(&parser, out, error);
    yaml_parser_delete(&parser);
    return rc;
}

} // namespace budyk
