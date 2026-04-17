// SPDX-License-Identifier: BSD-3-Clause
#include "rules/lua_bindings.h"
void budyk_lua_bind_sample(lua_State*, const budyk::Sample&) {
    // TODO: push cpu.total_percent, mem.available_percent, etc. as Lua globals
}
