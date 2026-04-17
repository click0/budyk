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

if(LUA_FOUND)
    add_executable(test_lua_engine unit/test_lua_engine.cpp)
    target_link_libraries(test_lua_engine PRIVATE budyk_rules budyk_core ${LUA_LIBRARIES})
    target_include_directories(test_lua_engine PRIVATE ${CMAKE_SOURCE_DIR}/src ${LUA_INCLUDE_DIR})
    add_test(NAME test_lua_engine COMMAND test_lua_engine)
endif()
