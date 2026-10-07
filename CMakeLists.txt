# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 budyk contributors

cmake_minimum_required(VERSION 3.22)
project(budyk VERSION 0.6.5 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No exceptions, no RTTI — minimal footprint
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

# A plain `cmake -B build` used to produce an unoptimised binary with
# no NDEBUG, and _FORTIFY_SOURCE did nothing in it. Default to Release,
# as a build to install should be; pass -DCMAKE_BUILD_TYPE=Debug (or
# RelWithDebInfo) for debugging. Multi-config generators choose at build
# time and are left alone.
# Sanitizer builds default to RelWithDebInfo instead: optimised, with
# the debug info their reports need.
get_property(BUDYK_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT BUDYK_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    if(ENABLE_SANITIZERS OR ENABLE_TSAN)
        set(BUDYK_DEFAULT_BUILD_TYPE RelWithDebInfo)
    else()
        set(BUDYK_DEFAULT_BUILD_TYPE Release)
    endif()
    set(CMAKE_BUILD_TYPE ${BUDYK_DEFAULT_BUILD_TYPE} CACHE STRING
        "Build type: Release (default), RelWithDebInfo, Debug, MinSizeRel" FORCE)
endif()

# Security hardening.
# _FORTIFY_SOURCE needs optimisation; Debug builds go without it. Some
# toolchains (Ubuntu's GCC) already define it, at another level: undefine
# first, so the command line decides and no redefinition warning trips
# -Werror.
add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>")
add_compile_options(-fstack-protector-strong)
# Stack-clash and control-flow protection where the compiler has them
# (GCC 8+ / clang 11+; -fcf-protection on x86 only), and a read-only GOT
# with eager binding where the linker supports it.
include(CheckCCompilerFlag)
include(CheckLinkerFlag)
check_c_compiler_flag(-fstack-clash-protection BUDYK_HAVE_STACK_CLASH)
if(BUDYK_HAVE_STACK_CLASH)
    add_compile_options(-fstack-clash-protection)
endif()
check_c_compiler_flag(-fcf-protection=full BUDYK_HAVE_CF_PROTECTION)
if(BUDYK_HAVE_CF_PROTECTION)
    add_compile_options(-fcf-protection=full)
endif()
check_linker_flag(C "LINKER:-z,relro,-z,now" BUDYK_HAVE_RELRO_NOW)
if(BUDYK_HAVE_RELRO_NOW)
    add_link_options("LINKER:-z,relro,-z,now")
endif()

# Warnings are always on. CI adds -DENABLE_WERROR=ON so a new warning
# fails the build; a local or distro build with a newer compiler that
# warns about something new still builds.
add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion)
option(ENABLE_WERROR "Treat compiler warnings as errors (used by CI)" OFF)
if(ENABLE_WERROR)
    add_compile_options(-Werror)
endif()

# Platform detection
include(cmake/platform.cmake)

# Options
option(ENABLE_LLM "Enable LLM API client for AI rule suggestions (Tier B)" OFF)
option(ENABLE_EXEC "Enable exec() action in Lua rules (security-sensitive)" OFF)
option(BUILD_TESTS "Build test suite" ON)
option(STATIC_LINK "Static linking for single-binary deployment" ON)

# -static on the final link only works if every dependency resolves to a
# static archive. By default find_library / find_package prefer the shared
# object (liblua5.4.so, libargon2.so, ...), and the link dies with
# "attempted static link of dynamic object". Restrict the search to .a
# before any find_* runs; the setting is inherited by src/ and tests/.
#
# -static applies to every executable, tests included: once dependencies
# are archives, a dynamic test binary can't take them (glibc's libm.a,
# pulled in via LUA_LIBRARIES, only links into a static executable), and
# the tests then exercise the same link mode that ships.
if(STATIC_LINK)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    add_link_options(-static)
    # Static ncurses on Debian/Ubuntu (and FreeBSD) is split into
    # libncurses + libtinfo; the shared lib pulls tinfo in via DT_NEEDED,
    # the archive does not. With NEED_NCURSES, FindCurses probes the
    # ncurses archive for cbreak() and appends libtinfo when it's missing.
    set(CURSES_NEED_NCURSES TRUE)
    message(STATUS "STATIC_LINK=ON: resolving dependencies to static archives (.a) only; "
                   "pass -DSTATIC_LINK=OFF to link against shared libraries")
endif()

# AddressSanitizer + UndefinedBehaviorSanitizer, for CI and debugging. Any
# report aborts the process (-fno-sanitize-recover), so a test that hits
# UB or a memory error fails instead of printing and passing. vptr is left
# out because the project builds with -fno-rtti, which it needs.
option(ENABLE_SANITIZERS "Build with ASan + UBSan (needs STATIC_LINK=OFF)" OFF)
if(ENABLE_SANITIZERS)
    if(STATIC_LINK)
        message(FATAL_ERROR "ENABLE_SANITIZERS needs -DSTATIC_LINK=OFF: "
                            "ASan can't be linked into a static binary")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-sanitize=vptr
                        -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "Sanitizers: address, undefined")
endif()

# ThreadSanitizer, for CI: two threads (collector tick, HTTP) plus the
# Workers share the hot buffer, the rings, the hub and the session
# store. Not combinable with ASan; -fno-sanitize-recover so a report
# fails the test.
option(ENABLE_TSAN "Build with ThreadSanitizer (needs STATIC_LINK=OFF)" OFF)
if(ENABLE_TSAN)
    if(STATIC_LINK)
        message(FATAL_ERROR "ENABLE_TSAN needs -DSTATIC_LINK=OFF")
    endif()
    if(ENABLE_SANITIZERS)
        message(FATAL_ERROR "ENABLE_TSAN and ENABLE_SANITIZERS cannot be combined")
    endif()
    add_compile_options(-fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
    # GCC 12/13 report -Warray-bounds inside libstdc++'s std::string when
    # TSan instrumentation meets optimisation (a vector<string> growing
    # in alert.cpp, for one); the code is fine and other builds keep the
    # warning fatal. Leave it visible here, not an error.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-error=array-bounds)
    endif()
    message(STATUS "Sanitizers: thread")
endif()

# Find dependencies
find_package(PkgConfig QUIET)

# Lua 5.4 — rule engine. No vendored copy is shipped, so a missing Lua must
# fail here rather than surface later as a missing lauxlib.h.
find_package(Lua 5.4 QUIET)
if(NOT LUA_FOUND)
    message(FATAL_ERROR "Lua 5.4 not found (install liblua5.4-dev on Debian/Ubuntu, "
                        "lua54 on FreeBSD; with STATIC_LINK=ON its static archive is required)")
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
