// SPDX-License-Identifier: BSD-3-Clause
#include "config/config.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

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
        assert(c.auth_enabled      == true);
        assert(std::strncmp(c.password_hash, "$argon2id$", 10) == 0);
    }

    // 5. Malformed YAML rejected.
    {
        Config c;
        assert(config_load_string("listen: [unterminated", &c) != 0);
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

    // 7g. security.ssh_audit — gate + path + interval all parse.
    {
        Config c;
        const char* y =
            "security:\n"
            "  ssh_audit:\n"
            "    enabled: true\n"
            "    path: /var/log/secure\n"
            "    interval: 30\n";
        assert(config_load_string(y, &c) == 0);
        assert(c.ssh_audit_enabled == true);
        assert(std::strcmp(c.ssh_audit_path, "/var/log/secure") == 0);
        assert(c.ssh_audit_interval_sec == 30);
    }

    // 7g-bis. security.file_watch — gate + paths list parse.
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
        assert(c.ssh_audit_enabled == false);
        assert(std::strcmp(c.ssh_audit_path, "/var/log/auth.log") == 0);
        assert(c.ssh_audit_interval_sec == 60);
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

    std::printf("test_config: PASS\n");
    return 0;
}
