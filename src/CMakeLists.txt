# SPDX-License-Identifier: BSD-3-Clause

# Core library (C++17, no I/O)
add_library(budyk_core STATIC
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
    # libkvm is in base — needed by memory collector for swap accounting.
    target_link_libraries(budyk_collector PUBLIC kvm)
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
    rules/yaml_compat.cpp
)
target_link_libraries(budyk_rules PRIVATE budyk_core ${LUA_LIBRARIES})
target_include_directories(budyk_rules PRIVATE ${LUA_INCLUDE_DIR})

# AI suggestions
add_library(budyk_ai STATIC
    ai/baseline.cpp
    ai/suggest.cpp
)
target_link_libraries(budyk_ai PRIVATE budyk_core budyk_storage)
if(ENABLE_LLM)
    target_sources(budyk_ai PRIVATE ai/llm_client.cpp)
    target_compile_definitions(budyk_ai PRIVATE BUDYK_LLM_ENABLED)
endif()

# Web server
add_library(budyk_web STATIC
    web/server.cpp
    web/ws_hub.cpp
    web/auth.cpp
)
target_link_libraries(budyk_web PRIVATE budyk_core)
# Argon2id for password hashing (spec §3.2). Required at v1.0.
find_library(ARGON2_LIB argon2 REQUIRED)
find_path(ARGON2_INCLUDE_DIR argon2.h REQUIRED)
target_link_libraries(budyk_web PRIVATE ${ARGON2_LIB})
target_include_directories(budyk_web PRIVATE ${ARGON2_INCLUDE_DIR})
# TODO: link mongoose or libwebsockets

# TUI
add_library(budyk_tui STATIC
    tui/tui.cpp
)
target_link_libraries(budyk_tui PRIVATE budyk_core)
# ncurses from FreeBSD base
find_package(Curses QUIET)
if(CURSES_FOUND)
    target_link_libraries(budyk_tui PRIVATE ${CURSES_LIBRARIES})
endif()

# Config
add_library(budyk_config STATIC
    config/config.cpp
)
target_link_libraries(budyk_config PRIVATE budyk_core)
find_library(YAML_LIB yaml REQUIRED)
find_path(YAML_INCLUDE_DIR yaml.h REQUIRED)
target_link_libraries(budyk_config PRIVATE ${YAML_LIB})
target_include_directories(budyk_config PRIVATE ${YAML_INCLUDE_DIR})

# Main binary
add_executable(budyk main.cpp)
target_link_libraries(budyk PRIVATE
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
    ${BUDYK_PLATFORM_LIBS}
    ${LUA_LIBRARIES}
)

if(STATIC_LINK)
    target_link_options(budyk PRIVATE -static)
endif()
