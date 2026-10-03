# SPDX-License-Identifier: BSD-3-Clause

# Core library (C++17, no I/O)
add_library(budyk_core STATIC
    core/json_text.cpp
    core/sample.cpp
    core/codec.cpp
)
target_include_directories(budyk_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Platform compile-time guard. Source files (notably src/main.cpp) use
# BUDYK_LINUX / BUDYK_FREEBSD to pick the right collector ABI.
if(BUDYK_PLATFORM STREQUAL "linux")
    target_compile_definitions(budyk_core PUBLIC BUDYK_LINUX)
elseif(BUDYK_PLATFORM STREQUAL "freebsd")
    target_compile_definitions(budyk_core PUBLIC BUDYK_FREEBSD)
endif()

# Platform collector (C)
if(BUDYK_PLATFORM STREQUAL "freebsd")
    add_library(budyk_collector STATIC
        collector/freebsd/cpu.c
        collector/freebsd/memory.c
        collector/freebsd/network.c
        collector/freebsd/disk.c
        collector/freebsd/system.c
    )
    # libkvm + libdevstat both in base. kvm for swap accounting,
    # devstat for aggregate disk I/O in collector/freebsd/disk.c.
    # Listed in dependency order (devstat uses kvm) — it matters for .a.
    target_link_libraries(budyk_collector PUBLIC devstat kvm)
    if(STATIC_LINK)
        # libkvm.a calls into libelf (kvm_open → elf_begin, gelf_getehdr,
        # ...). libkvm.so pulls it in via DT_NEEDED; the archive doesn't.
        target_link_libraries(budyk_collector PUBLIC elf)
    endif()
elseif(BUDYK_PLATFORM STREQUAL "linux")
    add_library(budyk_collector STATIC
        collector/linux/cpu.c
        collector/linux/memory.c
        collector/linux/network.c
        collector/linux/disk.c
        collector/linux/system.c
    )
else()
    message(FATAL_ERROR "No collector for platform: ${BUDYK_PLATFORM}")
endif()
target_include_directories(budyk_collector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Scheduler
# Small shared helpers with I/O or a library behind them, which core/
# (pure) must not carry: private temp files, libyaml DOM lookups.
find_library(YAML_LIB_UTIL yaml REQUIRED)
find_path(YAML_INCLUDE_DIR_UTIL yaml.h REQUIRED)
add_library(budyk_util STATIC
    util/tmpfile.cpp
    util/yaml_dom.cpp
)
target_link_libraries(budyk_util PUBLIC ${YAML_LIB_UTIL})
target_include_directories(budyk_util PUBLIC ${YAML_INCLUDE_DIR_UTIL})
target_include_directories(budyk_util PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_library(budyk_scheduler STATIC
    scheduler/scheduler.cpp
)
target_link_libraries(budyk_scheduler PRIVATE budyk_core)

# Hot buffer
add_library(budyk_hot_buffer STATIC
    hot_buffer/hot_buffer.cpp
)
target_link_libraries(budyk_hot_buffer PRIVATE budyk_core)

# Storage engine
add_library(budyk_storage STATIC
    storage/ring_file.cpp
    storage/tier_manager.cpp
    storage/tier_aggregator.cpp
    storage/codec.cpp
)
target_link_libraries(budyk_storage PRIVATE budyk_core)

# Lua rule engine
add_library(budyk_rules STATIC
    rules/lua_engine.cpp
    rules/lua_bindings.cpp
    rules/lua_stdlib.cpp
    rules/exec_action.cpp
    rules/alert.cpp
    rules/worker.cpp
    rules/freeze.cpp
    rules/yaml_compat.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(budyk_rules PRIVATE budyk_core budyk_util ${LUA_LIBRARIES})
target_link_libraries(budyk_rules PUBLIC Threads::Threads)   # Worker threads
target_include_directories(budyk_rules PRIVATE ${LUA_INCLUDE_DIR})
# libyaml is in base and already used by budyk_config; needed here for
# the simple-YAML → Lua transpiler in rules/yaml_compat.cpp.
find_library(YAML_LIB_RULES yaml REQUIRED)
find_path(YAML_INCLUDE_DIR_RULES yaml.h REQUIRED)
target_link_libraries(budyk_rules PRIVATE ${YAML_LIB_RULES})
target_include_directories(budyk_rules PRIVATE ${YAML_INCLUDE_DIR_RULES})

# AI suggestions
add_library(budyk_ai STATIC
    ai/baseline.cpp
    ai/suggest.cpp
    ai/llm_client.cpp
)
target_link_libraries(budyk_ai PRIVATE budyk_core budyk_util budyk_storage)
# ENABLE_LLM is now an informational option only — llm_client invokes
# curl(1) at runtime, so there's no build-time TLS / HTTP dep to gate.
# The user opts in via `budyk suggest-rules --ai --api-key …`.
if(ENABLE_LLM)
    target_compile_definitions(budyk_ai PRIVATE BUDYK_LLM_ENABLED)
endif()

# Web server
add_library(budyk_web STATIC
    web/server.cpp
    web/ws_hub.cpp
    web/auth.cpp
    web/json.cpp
    web/http_util.cpp
    web/session.cpp
    web/login_limiter.cpp
    web/sha1_base64.cpp
    web/spa.cpp
)
target_link_libraries(budyk_web PRIVATE budyk_core)
# pthreads — HttpServer runs an accept loop on a worker std::thread.
find_package(Threads REQUIRED)
target_link_libraries(budyk_web PUBLIC Threads::Threads)
# Argon2id for password hashing (spec §3.2). Required at v1.0.
find_library(ARGON2_LIB argon2 REQUIRED)
find_path(ARGON2_INCLUDE_DIR argon2.h REQUIRED)
target_link_libraries(budyk_web PRIVATE ${ARGON2_LIB})
target_include_directories(budyk_web PRIVATE ${ARGON2_INCLUDE_DIR})
# The HTTP/1.1 and WebSocket server is hand-written (web/server.cpp,
# web/ws_hub.cpp); no third-party web library.

# TUI
add_library(budyk_tui STATIC
    tui/tui.cpp
)
target_link_libraries(budyk_tui PRIVATE budyk_core)
# ncurses from FreeBSD base
find_package(Curses QUIET)
if(CURSES_FOUND)
    target_link_libraries(budyk_tui PRIVATE ${CURSES_LIBRARIES})
    target_include_directories(budyk_tui PRIVATE ${CURSES_INCLUDE_DIRS})
    target_compile_definitions(budyk_tui PRIVATE BUDYK_HAVE_CURSES)
endif()

# Security — file change watcher (PanicMode-inspired)
add_library(budyk_security STATIC
    security/file_watcher.cpp
)
target_link_libraries(budyk_security PRIVATE budyk_core)

# Config
add_library(budyk_config STATIC
    config/config.cpp
)
target_link_libraries(budyk_config PRIVATE budyk_core budyk_util)
find_library(YAML_LIB yaml REQUIRED)
find_path(YAML_INCLUDE_DIR yaml.h REQUIRED)
target_link_libraries(budyk_config PRIVATE ${YAML_LIB})
target_include_directories(budyk_config PRIVATE ${YAML_INCLUDE_DIR})

# Main binary
# The daemon (serve loop, HTTP routes, collector bridge) and the one-shot
# commands, as a library so the routes can be unit-tested without a
# socket (tests/unit/test_router.cpp). main.cpp only dispatches.
add_library(budyk_app STATIC
    daemon/collect.cpp
    daemon/router.cpp
    daemon/serve.cpp
    cli/commands.cpp
)
target_link_libraries(budyk_app PUBLIC
    budyk_core
    budyk_collector
    budyk_scheduler
    budyk_hot_buffer
    budyk_storage
    budyk_rules
    budyk_ai
    budyk_web
    budyk_tui
    budyk_config
    budyk_security
    ${BUDYK_PLATFORM_LIBS}
    ${LUA_LIBRARIES}
)

add_executable(budyk main.cpp)
# One source of truth for the version string (`budyk version`,
# /api/health): project(VERSION) in the top-level CMakeLists.txt.
target_compile_definitions(budyk PRIVATE BUDYK_VERSION="${PROJECT_VERSION}")
target_link_libraries(budyk PRIVATE budyk_app)
# STATIC_LINK's -static is applied to all executables in the top-level
# CMakeLists.txt, next to the static-archive lookup it depends on.
