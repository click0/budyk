// SPDX-License-Identifier: BSD-3-Clause
#include "config/config.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

using namespace budyk;

static bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

// What config_load_string() writes to stderr on `yaml`; its return code
// in *rc.
static std::string stderr_of(const char* yaml, Config* c, int* rc = nullptr,
                             std::string* error = nullptr) {
    char path[] = "/tmp/budyk_cfg_err_XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    std::fflush(stderr);
    const int saved = dup(2);
    assert(saved >= 0 && dup2(fd, 2) == 2);
    const int r = config_load_string(yaml, c, error);
    std::fflush(stderr);
    assert(dup2(saved, 2) == 2);
    close(saved);
    if (rc != nullptr) *rc = r;
    std::string out;
    char buf[4096];
    ssize_t n = 0;
    assert(lseek(fd, 0, SEEK_SET) == 0);
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fd);
    unlink(path);
    return out;
}

static bool has(const std::string& s, const char* what) {
    return s.find(what) != std::string::npos;
}

int main(int argc, char** argv) {
    // 1. Defaults applied when YAML is empty.
    {
        Config c;
        assert(config_load_string("{}", &c) == 0);
        assert(std::strcmp(c.listen_addr, "127.0.0.1") == 0);
        assert(c.listen_port == 8080);
        assert(c.scheduler.l1_interval_sec == 300);
        assert(c.scheduler.hysteresis_sec  == 300);
        assert(c.scheduler.escalate_after_sec == 300);
        assert(near(c.scheduler.escalation_load_1m, 4.0));
    }

    // 2. Top-level override.
    {
        Config c;
        const char* y =
            "listen: 0.0.0.0\n"
            "port: 9090\n"
            "data_dir: /tmp/budyk\n";
        assert(config_load_string(y, &c) == 0);
        assert(std::strcmp(c.listen_addr, "0.0.0.0") == 0);
        assert(c.listen_port == 9090);
        assert(std::strcmp(c.data_dir, "/tmp/budyk") == 0);
    }

    // 3. collection: l1 / l2 / l3 / hot_buffer.
    {
        Config c;
        const char* y =
            "collection:\n"
            "  l1: { interval: 600 }\n"
            "  l2:\n"
            "    interval: 45\n"
            "    always_on: true\n"
            "    hysteresis: 120\n"
            "    escalate_after: 0\n"
            "    escalation_thresholds:\n"
            "      load_1m: 2.5\n"
            "      cpu_percent: 75\n"
            "      swap_used_percent: 30\n"
            "  l3: { interval: 2, grace_period: 10 }\n"
            "  hot_buffer: { capacity: 600, warm_grace: 45 }\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.scheduler.l1_interval_sec == 600);
        assert(c.scheduler.l2_interval_sec == 45);
        assert(c.scheduler.l2_always_on    == true);
        assert(c.scheduler.hysteresis_sec  == 120);
        assert(c.scheduler.escalate_after_sec == 0);
        assert(near(c.scheduler.escalation_load_1m,     2.5));
        assert(near(c.scheduler.escalation_cpu_percent, 75.0));
        assert(near(c.scheduler.escalation_swap_percent, 30.0));
        assert(c.scheduler.l3_interval_sec  == 2);
        assert(c.scheduler.grace_period_sec == 10);
        assert(c.hot_buffer_capacity        == 600);
        assert(c.hot_buffer_warm_grace      == 45);
    }

    // 4. storage, rules, web.auth.
    {
        Config c;
        const char* y =
            "storage:\n"
            "  tier1_max_mb: 500\n"
            "  tier2_max_mb: 200\n"
            "  tier3_max_mb: 100\n"
            "rules:\n"
            "  path: /etc/budyk/rules.lua\n"
            "  enable_exec: true\n"
            "  limits:\n"
            "    instructions: 250000\n"
            "    memory_mb: 4\n"
            "web:\n"
            "  auth:\n"
            "    enabled: true\n"
            "    password_hash: $argon2id$v=19$dummy\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.tier1_max_mb == 500);
        assert(c.tier2_max_mb == 200);
        assert(c.tier3_max_mb == 100);
        assert(std::strcmp(c.rules_path, "/etc/budyk/rules.lua") == 0);
        assert(c.rules_enable_exec == true);
        assert(c.rules_instruction_limit == 250000);
        assert(c.rules_memory_mb         == 4);
        assert(c.auth_enabled      == true);
        assert(std::strncmp(c.password_hash, "$argon2id$", 10) == 0);
        assert(c.auth_max_login_failures == 5 && c.auth_login_window_sec == 60);   // defaults
        Config t;
        assert(config_load_string(
            "web: { auth: { max_login_failures: 3, login_window: 120 } }\n", &t) == 0);
        assert(t.auth_max_login_failures == 3 && t.auth_login_window_sec == 120);
        Config tb;
        assert(config_load_string(
            "web: { auth: { max_login_failures: 0, login_window: 999999 } }\n", &tb) == 0);
        assert(tb.auth_max_login_failures == 5 && tb.auth_login_window_sec == 60);

        // Defaults when the block is absent; out-of-range values fall
        // back to the defaults like every other clamped number.
        Config d;
        assert(config_load_string("rules: {}\n", &d) == 0);
        assert(d.rules_instruction_limit == 1000000);
        assert(d.rules_memory_mb         == 16);
        Config bad;
        assert(config_load_string(
            "rules: { limits: { instructions: 5, memory_mb: 0 } }\n", &bad) == 0);
        assert(bad.rules_instruction_limit == 1000000);
        assert(bad.rules_memory_mb         == 16);
    }

    // 5. Malformed YAML rejected.
    {
        Config c;
        assert(config_load_string("listen: [unterminated", &c) != 0);
    }

    // 5b. A file that cannot be opened: the reason comes back in words.
    {
        Config c;
        std::string err;
        assert(config_load("/nonexistent/budyk.yaml", &c, &err) == -2);
        assert(err.find("No such file") != std::string::npos);
        assert(config_load(nullptr, &c, &err) == -1);
        assert(!err.empty());
    }

    // 6. Null args rejected.
    {
        Config c;
        assert(config_load_string(nullptr, &c)   != 0);
        assert(config_load_string("{}", nullptr) != 0);
        assert(config_load(nullptr, &c)          != 0);
    }

    // 7a. Nested rules.exec block — preferred spelling with an allowlist.
    {
        Config c;
        const char* y =
            "rules:\n"
            "  path: /etc/budyk/rules.lua\n"
            "  exec:\n"
            "    enabled: true\n"
            "    allow:\n"
            "      - /usr/local/bin/my-alerter\n"
            "      - /usr/bin/systemctl\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.rules_enable_exec == true);
        assert(c.rules_exec_allow.size() == 2);
        assert(c.rules_exec_allow[0] == "/usr/local/bin/my-alerter");
        assert(c.rules_exec_allow[1] == "/usr/bin/systemctl");
    }

    // 7a-ter. web.allowed_hosts: the names a reverse proxy passes in Host;
    //         none by default.
    {
        Config c;
        assert(c.web_allowed_hosts.empty());
        const char* y =
            "web:\n"
            "  allowed_hosts: [budyk.example.com, dash.lan]\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.web_allowed_hosts.size() == 2);
        assert(c.web_allowed_hosts[0] == "budyk.example.com" && c.web_allowed_hosts[1] == "dash.lan");
    }

    // 7a-bis. rules.persist_state + state_path parse; defaults are
    //         persist=true, empty path (derived from data_dir later).
    {
        Config c;
        assert(c.rules_persist_state == true);   // default
        assert(c.rules_state_path[0] == '\0');
        const char* y =
            "rules:\n"
            "  persist_state: false\n"
            "  state_path: /var/db/budyk/rules.tsv\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.rules_persist_state == false);
        assert(std::strcmp(c.rules_state_path, "/var/db/budyk/rules.tsv") == 0);
    }

    // 7a-ter. rules.path given as nothing, `~`, `null` or "" is no rules
    //         (a null kept the default, and the daemon loaded the rules
    //         file the FreeBSD package installs there); left out, the
    //         default stays.
    {
        for (const char* v : {"", " ~", " null", " \"\""}) {
            Config c;
            const std::string y = std::string("rules:\n  path:") + v + "\n";
            assert(config_load_string(y.c_str(), &c) == 0);
            assert(c.rules_path[0] == '\0');
        }
        Config c;
        assert(config_load_string("rules:\n  persist_state: true\n", &c) == 0);
        assert(std::strcmp(c.rules_path, "/usr/local/etc/budyk/rules.lua") == 0);
    }

    // 7b. Missing allow key leaves the vector at default (empty).
    {
        Config c;
        const char* y =
            "rules:\n"
            "  exec:\n"
            "    enabled: true\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.rules_enable_exec == true);
        assert(c.rules_exec_allow.empty());
    }

    // 7c. Empty allow list is respected (explicit reset).
    {
        Config c;
        const char* y =
            "rules:\n"
            "  exec:\n"
            "    allow: []\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.rules_exec_allow.empty());
    }

    // 7d. Legacy flat `enable_exec` still works alongside nested allow list.
    {
        Config c;
        const char* y =
            "rules:\n"
            "  enable_exec: true\n"
            "  exec:\n"
            "    allow:\n"
            "      - /bin/true\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.rules_enable_exec == true);
        assert(c.rules_exec_allow.size() == 1);
        assert(c.rules_exec_allow[0] == "/bin/true");
    }

    // 7e. rules.freeze — gate + allowlist parse correctly.
    {
        Config c;
        const char* y =
            "rules:\n"
            "  freeze:\n"
            "    enabled: true\n"
            "    allow:\n"
            "      - nginx\n"
            "      - redis-server\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.rules_enable_freeze == true);
        assert(c.rules_freeze_allow.size() == 2);
        assert(c.rules_freeze_allow[0] == "nginx");
        assert(c.rules_freeze_allow[1] == "redis-server");
    }

    // 7f. rules.freeze defaults: missing block leaves disabled + empty.
    {
        Config c;
        assert(config_load_string("rules: {}\n", &c) == 0);
        assert(c.rules_enable_freeze == false);
        assert(c.rules_freeze_allow.empty());
    }

    // 7g. security.file_watch — gate + paths list parse.
    {
        Config c;
        const char* y =
            "security:\n"
            "  file_watch:\n"
            "    enabled: true\n"
            "    paths:\n"
            "      - /etc/sudoers\n"
            "      - /etc/passwd\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.file_watch_enabled == true);
        assert(c.file_watch_paths.size() == 2);
        assert(c.file_watch_paths[0] == "/etc/sudoers");
        assert(c.file_watch_paths[1] == "/etc/passwd");
    }

    // 7h. Defaults survive an unrelated config block.
    {
        Config c;
        assert(config_load_string("rules: {}\n", &c) == 0);
        assert(c.file_watch_enabled == false);
        assert(c.file_watch_paths.empty());
    }

    // 8. alerts.channels — full mix of all five backend types.
    {
        Config c;
        const char* y =
            "alerts:\n"
            "  channels:\n"
            "    - name: ops-ntfy\n"
            "      type: ntfy\n"
            "      url: https://ntfy.sh\n"
            "      topic: budyk-alerts\n"
            "    - name: ops-tg\n"
            "      type: telegram\n"
            "      token: \"12345:AAA\"\n"
            "      topic: \"-1001234567\"\n"
            "    - name: ops-mail\n"
            "      type: smtp\n"
            "      url: smtps://smtp.example.com:465\n"
            "      from: alerts@example.com\n"
            "      topic: oncall@example.com\n"
            "      token: \"alerts@example.com:hunter2\"\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.alert_channels.size() == 3);
        assert(c.alert_channels[0].type == "ntfy");
        assert(c.alert_channels[0].url  == "https://ntfy.sh");
        assert(c.alert_channels[0].topic == "budyk-alerts");
        assert(c.alert_channels[1].type == "telegram");
        assert(c.alert_channels[1].token == "12345:AAA");
        assert(c.alert_channels[2].type == "smtp");
        assert(c.alert_channels[2].from == "alerts@example.com");
        assert(c.alert_channels[2].topic == "oncall@example.com");
    }

    // 9. alerts: a channel with no `type`, or one that is not a mapping,
    //    is left out — the dispatcher would have nothing to route it to —
    //    and said so.
    {
        Config c;
        const char* y =
            "alerts:\n"
            "  channels:\n"
            "    - name: typeless\n"
            "      url: https://example.com/\n"
            "    - just-a-name\n"
            "    - name: good\n"
            "      type: ntfy\n"
            "      url: https://ntfy.sh\n"
            "      topic: t\n";
        const std::string err = stderr_of(y, &c);
        assert(c.alert_channels.size() == 1);
        assert(c.alert_channels[0].name == "good");
        assert(has(err, "alerts.channels[0] (typeless): no type"));
        assert(has(err, "alerts.channels[1]: not a mapping"));
    }

    // 10. File load path: write a small YAML and parse it.
    {
        char tmpl[] = "/tmp/budyk_cfg_XXXXXX";
        int  fd = mkstemp(tmpl);
        assert(fd >= 0);
        const char* y = "port: 7070\n";
        std::FILE* f = fdopen(fd, "w");
        std::fwrite(y, 1, std::strlen(y), f);
        std::fclose(f);

        Config c;
        assert(config_load(tmpl, &c) == 0);
        assert(c.listen_port == 7070);
        std::remove(tmpl);
    }

    // N. collection.levels: valid levels get ids from kFirstCustomLevel in
    //    order; invalid ones are skipped (and logged) without affecting
    //    the rest.
    {
        Config c;
        const char* y =
            "collection:\n"
            "  levels:\n"
            "    - { name: burst, interval: 0.5, priority: 40, when: \"cpu.total_percent > 95\", hold: 30, storage_mb: 10 }\n"
            "    - { name: deep, interval: 1800, priority: 5 }\n"
            "    - { name: 'bad name', interval: 1, priority: 1 }\n"
            "    - { name: noint, priority: 1 }\n"
            "    - { name: fast, interval: 0.01, priority: 1 }\n"
            "    - { name: noprio, interval: 1 }\n"
            "    - { name: L3, interval: 1, priority: 1 }\n"
            "    - { name: DEEP, interval: 1, priority: 1 }\n"
            "    - { name: badhold, interval: 1, priority: 1, hold: -1 }\n"
            "    - { name: badmb, interval: 1, priority: 1, storage_mb: 0 }\n"
            "    - { name: badunit, interval: 30x, priority: 1 }\n"
            "    - { name: halfmin, interval: 30s, priority: 1, hold: 2m }\n";
        assert(config_load_string(y, &c) == 0);
        const auto& lv = c.scheduler.custom_levels;
        assert(lv.size() == 3);
        assert(lv[2].name == "halfmin" && lv[2].interval_ms == 30000 && lv[2].hold_sec == 120);
        assert(lv[0].id == kFirstCustomLevel && lv[0].name == "burst");
        assert(lv[0].interval_ms == 500 && lv[0].priority == 40);
        assert(lv[0].when == "cpu.total_percent > 95");
        assert(lv[0].hold_sec == 30 && lv[0].storage_mb == 10);
        assert(lv[1].id == kFirstCustomLevel + 1 && lv[1].name == "deep");
        assert(lv[1].interval_ms == 1800000 && lv[1].priority == 5);
        assert(lv[1].when.empty() && lv[1].hold_sec == 0 && lv[1].storage_mb == 50);
    }

    // N+0b. Numbers are read whole, durations with a unit or without:
    //       strtol() kept the digits before any text, so "5m" was 5 s,
    //       "1.5h" 1 s, "4294967596" 300 (truncated) and "nan" a threshold
    //       that never fires. A value that does not read keeps the default.
    {
        Config c;
        const Config def;
        const char* y =
            "port: 80a\n"
            "collection:\n"
            "  l1: { interval: 5m }\n"
            "  l2: { interval: 1.5m, hysteresis: 10min, escalate_after: 4294967596,\n"
            "        escalation_thresholds: { cpu_percent: nan, load_1m: 4.5 } }\n"
            "  l3: { interval: 2s, grace_period: 1h }\n"
            "  hot_buffer: { capacity: 10k, warm_grace: 1d }\n"
            "web:\n"
            "  auth: { login_window: 1.5s }\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.listen_port == def.listen_port);                         // "80a"
        assert(c.scheduler.l1_interval_sec == 300);
        assert(c.scheduler.l2_interval_sec == 90);
        assert(c.scheduler.hysteresis_sec == 600);
        assert(c.scheduler.escalate_after_sec == def.scheduler.escalate_after_sec);
        assert(c.scheduler.escalation_cpu_percent == def.scheduler.escalation_cpu_percent);
        assert(c.scheduler.escalation_load_1m == 4.5);
        assert(c.scheduler.l3_interval_sec == 2 && c.scheduler.grace_period_sec == 3600);
        assert(c.hot_buffer_capacity == def.hot_buffer_capacity);         // "10k"
        assert(c.hot_buffer_warm_grace == 86400);
        assert(c.auth_login_window_sec == def.auth_login_window_sec);     // not whole
    }

    // N+1. At most kMaxCustomLevels levels are accepted.
    {
        std::string y = "collection:\n  levels:\n";
        for (int i = 0; i < 20; ++i) {
            y += "    - { name: l" + std::to_string(i + 10) + ", interval: 1, priority: 1 }\n";
        }
        Config c;
        assert(config_load_string(y.c_str(), &c) == 0);
        assert(c.scheduler.custom_levels.size() == kMaxCustomLevels);
        assert(c.scheduler.custom_levels.back().id == kMaxLevelId);
    }

    // Out-of-range numbers fall back to their defaults: a 0-record hot
    // buffer used to crash the daemon with SIGFPE, a 0 s interval would
    // spin a core.
    {
        Config c;
        assert(config_load_string(
            "port: 70000\n"
            "collection:\n"
            "  l1: { interval: -5 }\n"
            "  l2: { escalate_after: -1 }\n"
            "  l3: { interval: 0, grace_period: -1 }\n"
            "  hot_buffer: { capacity: 0 }\n"
            "storage: { tier1_max_mb: 0 }\n", &c) == 0);
        const Config d;
        assert(c.listen_port                == d.listen_port);
        assert(c.scheduler.l1_interval_sec  == d.scheduler.l1_interval_sec);
        assert(c.scheduler.l3_interval_sec  == d.scheduler.l3_interval_sec);
        assert(c.scheduler.grace_period_sec == d.scheduler.grace_period_sec);
        assert(c.scheduler.escalate_after_sec == d.scheduler.escalate_after_sec);
        assert(c.hot_buffer_capacity        == d.hot_buffer_capacity);
        assert(c.tier1_max_mb               == d.tier1_max_mb);
    }

    // collection.mode and <level>.metrics: a set name or a list; an
    // unknown mode is adaptive, a bad list leaves the level its default.
    {
        auto same = [](const MetricSet& x, const MetricSet& y) {
            return x.groups == y.groups && x.detail == y.detail;
        };
        Config d;
        assert(d.scheduler.mode == CollectionMode::Adaptive);
        assert(same(d.scheduler.l1_metrics, metric_set_standard()));
        assert(same(d.scheduler.l2_metrics, metric_set_full()));
        assert(same(d.scheduler.l3_metrics, metric_set_full()));

        Config c;
        assert(config_load_string(
            "collection:\n"
            "  mode: on-demand\n"
            "  l1: { interval: 300, metrics: minimal }\n"
            "  l2: { metrics: [disk, net.interfaces] }\n"
            "  l3: { metrics: standard }\n"
            "  levels:\n"
            "    - { name: a, interval: 1, priority: 40, metrics: [tcp] }\n"
            "    - { name: b, interval: 1, priority: 41 }\n", &c) == 0);
        assert(c.scheduler.mode == CollectionMode::OnDemand);
        assert(c.scheduler.l1_interval_sec == 300);
        assert(same(c.scheduler.l1_metrics, metric_set_minimal()));
        assert(same(c.scheduler.l2_metrics,
                    MetricSet{kGroupsMinimal | kGroupDisk | kGroupNet, kDetailInterfaces}));
        assert(same(c.scheduler.l3_metrics, metric_set_standard()));
        assert(c.scheduler.custom_levels.size() == 2);
        assert(same(c.scheduler.custom_levels[0].metrics,
                    MetricSet{kGroupsMinimal | kGroupTcp, 0}));
        assert(same(c.scheduler.custom_levels[1].metrics, metric_set_full()));

        Config a;
        assert(config_load_string("collection: { mode: always }\n", &a) == 0);
        assert(a.scheduler.mode == CollectionMode::Always);
        assert(config_load_string("collection: { mode: adaptive }\n", &a) == 0);
        assert(a.scheduler.mode == CollectionMode::Adaptive);

        Config b;
        b.scheduler.mode = CollectionMode::Always;
        assert(config_load_string(
            "collection:\n"
            "  mode: sometimes\n"
            "  l1: { metrics: [cpu, dsik] }\n"
            "  l2: { metrics: { a: b } }\n"
            "  l3: { metrics: [[cpu]] }\n"
            "  levels:\n"
            "    - { name: a, interval: 1, priority: 40, metrics: Full }\n", &b) == 0);
        assert(b.scheduler.mode == CollectionMode::Adaptive);
        assert(same(b.scheduler.l1_metrics, metric_set_standard()));
        assert(same(b.scheduler.l2_metrics, metric_set_full()));
        assert(same(b.scheduler.l3_metrics, metric_set_full()));
        assert(b.scheduler.custom_levels.size() == 1);
        assert(same(b.scheduler.custom_levels[0].metrics, metric_set_full()));
    }

    // Booleans in any case, as YAML writes them (`True` from Python and
    // Ansible): `web.auth.enabled: True` used to leave the dashboard open
    // without a word. Anything else is reported and the default kept.
    {
        Config c;
        const std::string err = stderr_of(
            "collection: { l2: { always_on: Yes } }\n"
            "rules: { exec: { enabled: ture }, freeze: { enabled: ON }, persist_state: FALSE }\n"
            "security: { file_watch: { enabled: True } }\n"
            "web: { auth: { enabled: True, persist_sessions: Off } }\n", &c);
        assert(c.auth_enabled && !c.auth_persist_sessions);
        assert(c.scheduler.l2_always_on && c.rules_enable_freeze && c.file_watch_enabled);
        assert(!c.rules_persist_state);
        assert(!c.rules_enable_exec);
        assert(has(err, "rules.exec.enabled: 'ture' is not true or false; using false"));
        assert(err.find("budyk config:") == err.rfind("budyk config:"));   // that one only
    }

    // Lists: one string is a list of one (`allow: /usr/bin/systemctl`
    // left the exec allowlist empty, which lets exec() run any absolute
    // path); an item that is not a string is left out, a mapping leaves
    // the default, both said so; no value leaves the default quietly.
    {
        Config c;
        std::string err = stderr_of(
            "rules:\n"
            "  exec: { allow: /usr/local/bin/budyk-alerter }\n"
            "  freeze: { allow: [nginx, [redis], {a: b}] }\n"
            "web: { allowed_hosts: dash.lan }\n", &c);
        assert(c.rules_exec_allow.size() == 1 && c.rules_exec_allow[0] == "/usr/local/bin/budyk-alerter");
        assert(c.rules_freeze_allow.size() == 1 && c.rules_freeze_allow[0] == "nginx");
        assert(c.web_allowed_hosts.size() == 1 && c.web_allowed_hosts[0] == "dash.lan");
        assert(has(err, "rules.freeze.allow[1]: not a string; left out"));
        assert(has(err, "rules.freeze.allow[2]: not a string; left out"));

        Config k;
        k.rules_exec_allow = {"/keep"};
        k.file_watch_paths = {"/etc/keep"};
        err = stderr_of("rules: { exec: { allow: { a: b } } }\n"
                        "security:\n  file_watch:\n    paths:\n", &k);
        assert(k.rules_exec_allow.size() == 1 && k.rules_exec_allow[0] == "/keep");
        assert(k.file_watch_paths.size() == 1 && k.file_watch_paths[0] == "/etc/keep");
        assert(has(err, "rules.exec.allow: a mapping where a list belongs; ignored"));
        assert(err.find("budyk config:") == err.rfind("budyk config:"));
    }

    // An allowlist written but left with nothing usable allows nothing,
    // and says so: every item a mapping (`- path: /usr/bin/x`), or a
    // mapping for the list. It was left empty, and an empty exec
    // allowlist lets exec() run any absolute path. The entry it holds is
    // no absolute path and longer than any process name. `[]` still means
    // any; one usable item is the list.
    {
        Config c;
        std::string err = stderr_of(
            "rules:\n"
            "  exec: { allow: [ { path: /usr/local/bin/budyk-alerter } ] }\n"
            "  freeze: { allow: { name: nginx } }\n", &c);
        assert(c.rules_exec_allow.size() == 1 && c.rules_exec_allow[0][0] != '/');
        assert(c.rules_freeze_allow.size() == 1 && c.rules_freeze_allow[0].size() > 19);
        assert(has(err, "rules.exec.allow: no usable entry; it allows nothing"));
        assert(has(err, "rules.freeze.allow: no usable entry; it allows nothing"));
        Config m;
        err = stderr_of("rules: { exec: { allow: [ /usr/bin/logger, { path: /x } ] } }\n", &m);
        assert(m.rules_exec_allow.size() == 1 && m.rules_exec_allow[0] == "/usr/bin/logger");
        assert(!has(err, "allows nothing"));
        Config e;
        assert(stderr_of("rules: { exec: { allow: [] } }\n", &e).empty() && e.rules_exec_allow.empty());
        Config w;                                        // not an allowlist: left empty
        err = stderr_of("security: { file_watch: { paths: [ { p: /etc } ] } }\n", &w);
        assert(w.file_watch_paths.empty() && !has(err, "allows nothing"));
    }

    // A value longer than its field holds is reported and the setting
    // kept: it was cut to fit without a word (a 256-byte data_dir lost its
    // last letter and the rings went to a directory of that name; a cut
    // password hash let no one in). One that fits exactly is taken.
    {
        Config c;
        const std::string dir = "/" + std::string(299, 'd');
        const std::string hash = "$argon2id$" + std::string(290, 'h');
        const std::string y = "data_dir: " + dir + "\nweb: { auth: { password_hash: \"" + hash +
                              "\" } }\n";
        const std::string err = stderr_of(y.c_str(), &c);
        assert(std::strcmp(c.data_dir, Config{}.data_dir) == 0);
        assert(c.password_hash[0] == '\0');
        assert(has(err, "data_dir: 300 bytes, longer than the 255 it can hold; using '/var/db/budyk'"));
        assert(has(err, "web.auth.password_hash: 300 bytes, longer than the 255 it can hold"));
        Config f;
        const std::string fits = "/" + std::string(254, 'f');
        assert(stderr_of(("data_dir: " + fits + "\n").c_str(), &f).empty());
        assert(fits == f.data_dir);
    }

    // An optional field of a custom level, a level's metrics and the mode
    // left with no value are as if not there: `hold: ~` dropped the level,
    // `when: ~` was the Lua text `~`, `metrics: ~` an unknown set "~".
    {
        Config c;
        const std::string err = stderr_of(
            "collection:\n"
            "  mode: ~\n"
            "  l1: { metrics: ~ }\n"
            "  levels:\n"
            "    - { name: burst, interval: 1, priority: 40, when: ~, hold: ~, storage_mb: ~,"
            " metrics: ~ }\n", &c);
        if (!err.empty()) std::fprintf(stderr, "%s", err.c_str());
        assert(err.empty());
        assert(c.scheduler.mode == CollectionMode::Adaptive);
        assert(c.scheduler.custom_levels.size() == 1);
        const CustomLevel& lv = c.scheduler.custom_levels[0];
        assert(lv.when.empty() && lv.hold_sec == 0 && lv.storage_mb == 50);
    }

    // A key the loader does not read, one given twice, a section that is
    // not one and a value that is a list or a mapping are reported under
    // their full names (a typo such as `enable:` used to be ignored
    // without a word); the rest loads.
    {
        Config c;
        const std::string err = stderr_of(
            "port: 7001\n"
            "port: 7002\n"
            "listen: [127.0.0.1]\n"
            "storage: 5\n"
            "colection: { mode: always }\n"
            "collection:\n"
            "  l3: { interval: 2, intervall: 5 }\n"
            "  levels:\n"
            "    - { name: burst, interval: 1, priority: 40, colour: red }\n"
            "rules: { limits: { instructions: { n: 1 } } }\n"
            "alerts: { channels: { name: x } }\n"
            "web:\n"
            "  auth: { enable: true }\n", &c);
        assert(c.listen_port == 7001);
        assert(std::strcmp(c.listen_addr, "127.0.0.1") == 0);           // the default
        assert(c.scheduler.l3_interval_sec == 2);
        assert(c.scheduler.custom_levels.size() == 1);
        assert(!c.auth_enabled);
        assert(has(err, "budyk config: port: given twice; the first is read"));
        assert(has(err, "budyk config: listen: a list where a value belongs; ignored"));
        assert(has(err, "budyk config: storage: not a mapping of settings; ignored"));
        assert(has(err, "budyk config: colection: unknown key; ignored"));
        assert(has(err, "budyk config: collection.l3.intervall: unknown key; ignored"));
        assert(has(err, "budyk config: collection.levels[0].colour: unknown key; ignored"));
        assert(has(err, "budyk config: rules.limits.instructions: a mapping where a value belongs"));
        assert(has(err, "budyk config: alerts.channels: not a list; ignored"));
        assert(has(err, "budyk config: web.auth.enable: unknown key; ignored"));
    }

    // Every key the loader reads, once: not a word on stderr. Sections
    // left empty (their keys commented out) are fine too.
    {
        Config c;
        const std::string err = stderr_of(
            "listen: 127.0.0.1\nport: 8080\ndata_dir: /var/db/budyk\n"
            "collection:\n"
            "  mode: adaptive\n"
            "  l1: { interval: 300, metrics: standard }\n"
            "  l2: { interval: 30, always_on: false, hysteresis: 300, escalate_after: 300,\n"
            "        escalation_thresholds: { load_1m: 4, cpu_percent: 85, swap_used_percent: 50 },\n"
            "        metrics: full }\n"
            "  l3: { interval: 1, grace_period: 60, metrics: full }\n"
            "  levels:\n"
            "    - { name: b, interval: 0.5, priority: 40, when: \"cpu.total_percent > 95\",\n"
            "        hold: 120, storage_mb: 10, metrics: full }\n"
            "  hot_buffer: { capacity: 300, warm_grace: 60 }\n"
            "storage: { tier1_max_mb: 250, tier2_max_mb: 150, tier3_max_mb: 50 }\n"
            "rules:\n"
            "  path: /usr/local/etc/budyk/rules.lua\n"
            "  enable_exec: false\n"
            "  persist_state: true\n"
            "  state_path: /var/db/budyk/rule_state.tsv\n"
            "  exec: { enabled: false, allow: [/usr/bin/systemctl] }\n"
            "  limits: { instructions: 1000000, memory_mb: 16 }\n"
            "  freeze: { enabled: false, allow: [nginx] }\n"
            "alerts:\n"
            "  channels:\n"
            "    - { name: n, type: ntfy, url: \"https://ntfy.sh\", topic: t, token: x, from: y }\n"
            "security: { file_watch: { enabled: false, paths: [/etc/passwd] } }\n"
            "web:\n"
            "  allowed_hosts: [budyk.example.com]\n"
            "  auth: { enabled: false, password_hash: \"x\", persist_sessions: true,\n"
            "          max_login_failures: 5, login_window: 60 }\n", &c);
        if (!err.empty()) std::fprintf(stderr, "%s", err.c_str());
        assert(err.empty());
        Config e;
        assert(stderr_of("security:\nweb:\n  auth:\n", &e).empty());
    }

    // The example config in the tree (argv[1]) loads without a word.
    if (argc > 1) {
        std::FILE* f = std::fopen(argv[1], "re");
        assert(f != nullptr);
        std::string text;
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        Config c;
        int rc = -1;
        const std::string err = stderr_of(text.c_str(), &c, &rc);
        if (!err.empty()) std::fprintf(stderr, "%s", err.c_str());
        assert(rc == 0 && err.empty());
    }

    // A parse error says what and where (it was "YAML parse error" alone);
    // an empty file or one of comments only is the defaults (it was
    // refused); a top level that is not a mapping is refused in words.
    {
        Config c;
        std::string error;
        assert(config_load_string("port: 1\nlisten: \"open\n", &c, &error) == -3);
        assert(has(error, "line 2") || has(error, "line 3"));
        assert(has(error, "quoted scalar"));
        assert(config_load_string("a: b: c\n", &c, &error) == -3);
        assert(has(error, "line 1, column"));

        Config d;
        const Config def;
        assert(config_load_string("", &d, &error) == 0 && error.empty());
        assert(config_load_string("# nothing here yet\n\n# port: 1\n", &d, &error) == 0);
        assert(d.listen_port == def.listen_port);
        assert(config_load_string("- port: 1\n", &d, &error) == -4);
        assert(has(error, "not a mapping"));

        char tmpl[] = "/tmp/budyk_cfg_XXXXXX";
        const int fd = mkstemp(tmpl);
        assert(fd >= 0);
        const char* y = "web:\n  auth:\n    enabled: 'yes\n";
        assert(write(fd, y, std::strlen(y)) == static_cast<ssize_t>(std::strlen(y)));
        close(fd);
        assert(config_load(tmpl, &c, &error) == -3);
        assert(has(error, "line ") && has(error, "column "));
        std::remove(tmpl);
    }

    // The config a command reads without --config: /usr/local's, or on
    // Linux the packages' /etc/budyk/config.yaml when only that one is
    // there (`budyk fsck` on a .deb's install gave up on /usr/local).
    {
        const char* local = "/usr/local/etc/budyk/config.yaml";
        auto none = [](const char*) { return false; };
        auto both = [](const char*) { return true; };
        auto etc  = [](const char* p) { return std::strcmp(p, "/etc/budyk/config.yaml") == 0; };
        assert(std::strcmp(default_config_path(none), local) == 0);
        assert(std::strcmp(default_config_path(both), local) == 0);
#ifdef BUDYK_LINUX
        assert(std::strcmp(default_config_path(etc), "/etc/budyk/config.yaml") == 0);
#else
        assert(std::strcmp(default_config_path(etc), local) == 0);
#endif
    }

    std::printf("test_config: PASS\n");
    return 0;
}
