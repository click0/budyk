// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "scheduler/scheduler.h"

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

    bool  rules_enable_exec      = false;
};

// Load a YAML config into `out`, starting from defaults. Missing
// keys leave defaults in place. Returns 0 on success, negative on
// I/O or parse error.
int config_load        (const char* path, Config* out);
int config_load_string (const char* yaml, Config* out);

} // namespace budyk
