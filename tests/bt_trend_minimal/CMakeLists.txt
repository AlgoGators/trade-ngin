cmake_minimum_required(VERSION 3.10)
project(bt_trend_minimal_test)

add_executable(bt_trend_minimal_test bt_trend_minimal_test.cpp)

# No external dependencies, only the stub header
# target_include_directories(bt_trend_minimal_test PRIVATE .)

set_target_properties(bt_trend_minimal_test PROPERTIES CXX_STANDARD 17)
target_link_libraries(bt_trend_minimal_test pthread) 