// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Optional: transpile simple YAML rules into Lua watch() calls at load time.
int budyk_yaml_to_lua(const char* yaml_path, char* lua_buf, int lua_buf_size);
