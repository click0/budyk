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

    // Auth
    bool auth_enabled = false;
    char password_hash[256] = "";

    // TODO: storage sizes, hot_buffer capacity, etc.
};

int config_load(const char* path, Config* out);

} // namespace budyk
