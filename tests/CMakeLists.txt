# SPDX-License-Identifier: BSD-3-Clause
# TODO: add cmocka (C) and/or gtest (C++) tests

add_executable(test_codec unit/test_codec.cpp)
target_link_libraries(test_codec PRIVATE budyk_core)
target_include_directories(test_codec PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_codec COMMAND test_codec)

add_executable(test_hot_buffer unit/test_hot_buffer.cpp)
target_link_libraries(test_hot_buffer PRIVATE budyk_hot_buffer budyk_core)
target_include_directories(test_hot_buffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_hot_buffer COMMAND test_hot_buffer)

add_executable(test_scheduler unit/test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE budyk_scheduler budyk_core)
target_include_directories(test_scheduler PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_scheduler COMMAND test_scheduler)

add_executable(test_ring_file unit/test_ring_file.cpp)
target_link_libraries(test_ring_file PRIVATE budyk_storage budyk_core)
target_include_directories(test_ring_file PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_ring_file COMMAND test_ring_file)

add_executable(test_tier_aggregator unit/test_tier_aggregator.cpp)
target_link_libraries(test_tier_aggregator PRIVATE budyk_storage budyk_core)
target_include_directories(test_tier_aggregator PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_tier_aggregator COMMAND test_tier_aggregator)

add_executable(test_baseline unit/test_baseline.cpp)
target_link_libraries(test_baseline PRIVATE budyk_ai budyk_core)
target_include_directories(test_baseline PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_baseline COMMAND test_baseline)

add_executable(test_suggest unit/test_suggest.cpp)
target_link_libraries(test_suggest PRIVATE budyk_ai budyk_core)
target_include_directories(test_suggest PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_suggest COMMAND test_suggest)

add_executable(test_auth unit/test_auth.cpp)
target_link_libraries(test_auth PRIVATE budyk_web budyk_core)
target_include_directories(test_auth PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_auth COMMAND test_auth)

add_executable(test_config unit/test_config.cpp)
target_link_libraries(test_config PRIVATE budyk_config budyk_scheduler budyk_core)
target_include_directories(test_config PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_config COMMAND test_config)

if(BUDYK_PLATFORM STREQUAL "linux")
    add_executable(test_linux_collector unit/test_linux_collector.cpp)
    target_link_libraries(test_linux_collector PRIVATE budyk_collector)
    target_include_directories(test_linux_collector PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME test_linux_collector COMMAND test_linux_collector)
endif()

if(LUA_FOUND)
    add_executable(test_lua_engine unit/test_lua_engine.cpp)
    target_link_libraries(test_lua_engine PRIVATE budyk_rules budyk_core ${LUA_LIBRARIES})
    target_include_directories(test_lua_engine PRIVATE ${CMAKE_SOURCE_DIR}/src ${LUA_INCLUDE_DIR})
    add_test(NAME test_lua_engine COMMAND test_lua_engine)
endif()

add_executable(test_exec_action unit/test_exec_action.cpp)
target_link_libraries(test_exec_action PRIVATE budyk_rules budyk_core)
target_include_directories(test_exec_action PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_exec_action COMMAND test_exec_action)
