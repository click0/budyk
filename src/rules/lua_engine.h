// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "core/sample.h"

namespace budyk {

class LuaEngine {
public:
    int  init(bool enable_exec);
    void shutdown();
    int  load_file(const char* path);
    int  eval_tick(const Sample& s);  // run all watch() rules against current sample
    int  rule_count() const;
private:
    void* L_ = nullptr;  // lua_State*, opaque here
    bool  exec_enabled_ = false;
};

} // namespace budyk
