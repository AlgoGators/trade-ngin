cmake_minimum_required(VERSION 3.10)
project(trade_ngin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Arrow package
find_package(Arrow REQUIRED)

# Find libpqxx package
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${Arrow_INCLUDE_DIRS}
    ${PQXX_INCLUDE_DIRS}
)

# Link directories
link_directories(${PQXX_LIBRARY_DIRS})

# Add database test executable
add_executable(test_db 
    trade_ngin/data/test_trend_strategy.cpp
    trade_ngin/data/market_data.cpp
    trade_ngin/data/database_interface.cpp
    trade_ngin/data/portfolio.cpp
)

# Link against Arrow and libpqxx
target_link_libraries(test_db PRIVATE 
    Arrow::arrow_shared
    ${PQXX_LIBRARIES}
)