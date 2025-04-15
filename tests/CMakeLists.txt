cmake_minimum_required(VERSION 3.10)
project(memory_thread_tests)

# Enable testing
enable_testing()

# Find GTest
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# Add test executables
add_executable(valgrind_benchmark valgrind_benchmark.cpp)
add_executable(race_conditions race_conditions.cpp)
add_executable(minimal_thread_test minimal_thread_test.cpp)

# Link against GTest
target_link_libraries(valgrind_benchmark ${GTEST_LIBRARIES} pthread)
target_link_libraries(race_conditions ${GTEST_LIBRARIES} pthread)
target_link_libraries(minimal_thread_test pthread)

# Add tests
add_test(NAME ValgrindTest COMMAND valgrind_benchmark)
add_test(NAME RaceConditionTest COMMAND race_conditions)
add_test(NAME MinimalThreadTest COMMAND minimal_thread_test) 