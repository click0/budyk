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

add_executable(test_baseline unit/test_baseline.cpp)
target_link_libraries(test_baseline PRIVATE budyk_ai budyk_core)
target_include_directories(test_baseline PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_baseline COMMAND test_baseline)
