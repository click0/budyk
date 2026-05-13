// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "scheduler/scheduler.h"

#include <string>
#include <vector>

namespace budyk {

struct Config {
    char listen_addr[64] = "127.0.0.1";
    int  listen_port     = 8080;
    char data_dir[256]   = "/var/db/budyk";
    char rules_path[256] = "/usr/local/etc/budyk/rules.lua";

    SchedulerConfig scheduler;

    bool  auth_enabled        = false;
    char  password_hash[256]  = "";

    int   tier1_max_mb           = 250;
    int   tier2_max_mb           = 150;
    int   tier3_max_mb           = 50;

    int   hot_buffer_capacity    = 300;
    int   hot_buffer_warm_grace  = 60;

    bool                     rules_enable_exec = false;
    // Absolute paths that `exec()` is permitted to launch. Empty = any
    // absolute path allowed (still subject to path-traversal guard).
    std::vector<std::string> rules_exec_allow;

    bool                     rules_enable_freeze = false;
    // Process names (kernel `comm`) that freeze()/unfreeze() are
    // allowed to signal. Empty = no name-based gate (the engine-wide
    // enabled flag still applies).
    std::vector<std::string> rules_freeze_allow;

    // Alert channels — POD mirror of rules::AlertChannel. Kept here to
    // avoid pulling budyk_config into budyk_rules' dep graph. main.cpp
    // copies the fields into a rules::AlertChannel for each entry.
    struct AlertChannelConfig {
        std::string name;
        std::string type;     // ntfy / discord / telegram / smtp / twilio
        std::string url;
        std::string topic;
        std::string token;
        std::string from;
    };
    std::vector<AlertChannelConfig> alert_channels;
};

// Load a YAML config into `out`, starting from defaults. Missing
// keys leave defaults in place. Returns 0 on success, negative on
// I/O or parse error.
int config_load        (const char* path, Config* out);
int config_load_string (const char* yaml, Config* out);

} // namespace budyk
