// SPDX-License-Identifier: BSD-3-Clause
#include "config/config.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace budyk;

static bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

int main() {
    // 1. Defaults applied when YAML is empty.
    {
        Config c;
        assert(config_load_string("{}", &c) == 0);
        assert(std::strcmp(c.listen_addr, "127.0.0.1") == 0);
        assert(c.listen_port == 8080);
        assert(c.scheduler.l1_interval_sec == 300);
        assert(c.scheduler.hysteresis_sec  == 300);
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

    // 9. alerts: a channel with no `type` is silently dropped — the
    //    dispatcher would have nothing to route it to anyway.
    {
        Config c;
        const char* y =
            "alerts:\n"
            "  channels:\n"
            "    - name: typeless\n"
            "      url: https://example.com/\n"
            "    - name: good\n"
            "      type: ntfy\n"
            "      url: https://ntfy.sh\n"
            "      topic: t\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.alert_channels.size() == 1);
        assert(c.alert_channels[0].name == "good");
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
            "    - { name: '30s', interval: 30s, priority: 1 }\n";
        assert(config_load_string(y, &c) == 0);
        const auto& lv = c.scheduler.custom_levels;
        assert(lv.size() == 2);
        assert(lv[0].id == kFirstCustomLevel && lv[0].name == "burst");
        assert(lv[0].interval_ms == 500 && lv[0].priority == 40);
        assert(lv[0].when == "cpu.total_percent > 95");
        assert(lv[0].hold_sec == 30 && lv[0].storage_mb == 10);
        assert(lv[1].id == kFirstCustomLevel + 1 && lv[1].name == "deep");
        assert(lv[1].interval_ms == 1800000 && lv[1].priority == 5);
        assert(lv[1].when.empty() && lv[1].hold_sec == 0 && lv[1].storage_mb == 50);
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
            "  l3: { interval: 0, grace_period: -1 }\n"
            "  hot_buffer: { capacity: 0 }\n"
            "storage: { tier1_max_mb: 0 }\n", &c) == 0);
        const Config d;
        assert(c.listen_port                == d.listen_port);
        assert(c.scheduler.l1_interval_sec  == d.scheduler.l1_interval_sec);
        assert(c.scheduler.l3_interval_sec  == d.scheduler.l3_interval_sec);
        assert(c.scheduler.grace_period_sec == d.scheduler.grace_period_sec);
        assert(c.hot_buffer_capacity        == d.hot_buffer_capacity);
        assert(c.tier1_max_mb               == d.tier1_max_mb);
    }

    std::printf("test_config: PASS\n");
    return 0;
}
