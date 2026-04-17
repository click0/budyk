// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_engine.h"
// #include <lua.h>
// #include <lauxlib.h>
// #include <lualib.h>

namespace budyk {

int  LuaEngine::init(bool enable_exec) {
    exec_enabled_ = enable_exec;
    // TODO: lua_newstate, sandbox setup, register builtins
    return -1;
}
void LuaEngine::shutdown() { /* TODO: lua_close */ }
int  LuaEngine::load_file(const char*) { return -1; /* TODO */ }
int  LuaEngine::eval_tick(const Sample&) { return -1; /* TODO */ }
int  LuaEngine::rule_count() const { return 0; }

} // namespace budyk
