cmake_minimum_required(VERSION 3.10)
project(trade_ngin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Arrow REQUIRED)
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

# Add executables
add_executable(test_db trade_ngin/data/test_trend_strategy.cpp 
                      trade_ngin/data/market_data.cpp 
                      trade_ngin/data/database_interface.cpp)

add_executable(mock_trading trade_ngin/data/run_mock_trading.cpp
                           trade_ngin/data/market_data.cpp
                           trade_ngin/data/database_interface.cpp)

# Link libraries
target_link_libraries(test_db PRIVATE Arrow::arrow_shared ${PQXX_LIBRARIES})
target_link_libraries(mock_trading PRIVATE Arrow::arrow_shared ${PQXX_LIBRARIES})