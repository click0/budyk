# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 budyk contributors

cmake_minimum_required(VERSION 3.22)
project(budyk VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No exceptions, no RTTI — minimal footprint
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

# Security hardening
add_compile_options(-D_FORTIFY_SOURCE=2 -fstack-protector-strong)

# Platform detection
include(cmake/platform.cmake)

# Options
option(ENABLE_LLM "Enable LLM API client for AI rule suggestions (Tier B)" OFF)
option(ENABLE_EXEC "Enable exec() action in Lua rules (security-sensitive)" OFF)
option(BUILD_TESTS "Build test suite" ON)
option(STATIC_LINK "Static linking for single-binary deployment" ON)

# Find dependencies
find_package(PkgConfig QUIET)

# Lua 5.4 — rule engine
find_package(Lua 5.4 QUIET)
if(NOT LUA_FOUND)
    message(STATUS "Lua 5.4 not found, will use vendored copy from third_party/")
    # TODO: add_subdirectory(third_party/lua)
endif()

# Sources
add_subdirectory(src)

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install
install(TARGETS budyk DESTINATION bin)
install(FILES config.example.yaml DESTINATION etc/budyk)
install(DIRECTORY rules/ DESTINATION share/budyk/rules)
install(FILES docs/budyk.8 DESTINATION man/man8)

message(STATUS "")
message(STATUS "  budyk ${PROJECT_VERSION}")
message(STATUS "  Platform:    ${BUDYK_PLATFORM}")
message(STATUS "  LLM:         ${ENABLE_LLM}")
message(STATUS "  exec():      ${ENABLE_EXEC}")
message(STATUS "  Static:      ${STATIC_LINK}")
message(STATUS "")
