# SPDX-License-Identifier: BSD-3-Clause

# The tests are plain assert()s, many of which call the code under test.
# A Release / RelWithDebInfo build defines NDEBUG, which would compile
# those calls away and let every test pass without running anything.
# Keep assert() live in the tests whatever the build type.
add_compile_options(-UNDEBUG)

# budyk_add_test(<name> LIBS <lib>... [INCLUDES <dir>...] [ARGS <arg>...])
#
# One unit test: unit/<name>.cpp built into <name>, linked against LIBS,
# with src/ (plus INCLUDES) on the include path, registered with ctest
# as <name> and run with ARGS.
function(budyk_add_test name)
    cmake_parse_arguments(T "" "" "LIBS;INCLUDES;ARGS" ${ARGN})
    add_executable(${name} unit/${name}.cpp)
    target_link_libraries(${name} PRIVATE ${T_LIBS})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src ${T_INCLUDES})
    add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
endfunction()

# core, storage, scheduler, hot buffer
budyk_add_test(test_codec           LIBS budyk_core)
budyk_add_test(test_parse_int       LIBS budyk_core)
budyk_add_test(test_storage_codec   LIBS budyk_storage budyk_core)
budyk_add_test(test_hot_buffer      LIBS budyk_hot_buffer budyk_core)
budyk_add_test(test_scheduler       LIBS budyk_scheduler budyk_core)
budyk_add_test(test_ring_file       LIBS budyk_storage budyk_core)
budyk_add_test(test_tier_aggregator LIBS budyk_storage budyk_core)
budyk_add_test(test_tier_manager    LIBS budyk_storage budyk_core)

# AI suggestions
budyk_add_test(test_baseline        LIBS budyk_ai budyk_core)
budyk_add_test(test_suggest         LIBS budyk_ai budyk_core)
budyk_add_test(test_llm_client      LIBS budyk_ai budyk_core)

# web
budyk_add_test(test_auth            LIBS budyk_web budyk_core)
budyk_add_test(test_http_server     LIBS budyk_web budyk_core)
budyk_add_test(test_json            LIBS budyk_web budyk_core)
budyk_add_test(test_session         LIBS budyk_web budyk_core)
budyk_add_test(test_login_limiter   LIBS budyk_web budyk_core)
budyk_add_test(test_ws              LIBS budyk_web budyk_core)
budyk_add_test(test_http_util       LIBS budyk_web budyk_core)

budyk_add_test(test_config          LIBS budyk_config budyk_scheduler budyk_core)

# platform collectors
if(BUDYK_PLATFORM STREQUAL "linux")
    budyk_add_test(test_linux_collector   LIBS budyk_collector)
endif()
if(BUDYK_PLATFORM STREQUAL "freebsd")
    budyk_add_test(test_freebsd_collector LIBS budyk_collector)
endif()

# rules
if(LUA_FOUND)
    budyk_add_test(test_lua_engine
        LIBS budyk_rules budyk_security budyk_core ${LUA_LIBRARIES}
        INCLUDES ${LUA_INCLUDE_DIR})

    # Spec §5 item 13: 100 rules per tick within 1 ms (median). Sanitizer
    # and coverage instrumentation slow the VM down several times over,
    # so those builds only report the timing.
    set(RULE_PERF_BUDGET_US 1000)
    if(ENABLE_SANITIZERS OR ENABLE_TSAN OR CMAKE_CXX_FLAGS MATCHES "--coverage")
        set(RULE_PERF_BUDGET_US 0)
    endif()
    budyk_add_test(test_rule_perf
        LIBS budyk_rules budyk_security budyk_core ${LUA_LIBRARIES}
        INCLUDES ${LUA_INCLUDE_DIR}
        ARGS ${RULE_PERF_BUDGET_US})

    budyk_add_test(test_yaml_rules
        LIBS budyk_rules budyk_core ${LUA_LIBRARIES}
        INCLUDES ${LUA_INCLUDE_DIR})
endif()
budyk_add_test(test_exec_action     LIBS budyk_rules budyk_core)
budyk_add_test(test_worker          LIBS budyk_rules budyk_core)
budyk_add_test(test_alert           LIBS budyk_rules budyk_core)
budyk_add_test(test_freeze          LIBS budyk_rules budyk_core)

budyk_add_test(test_file_watcher    LIBS budyk_security budyk_core)
budyk_add_test(test_util            LIBS budyk_util budyk_core)
# The daemon's routes, through make_router(): needs the whole app library.
budyk_add_test(test_router          LIBS budyk_app)
budyk_add_test(test_tui             LIBS budyk_tui budyk_core)
