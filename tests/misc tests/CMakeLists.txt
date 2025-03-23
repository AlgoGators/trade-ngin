# Set build type to Release
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

cmake_minimum_required(VERSION 3.10)
project(trade_ngin)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure spdlog
add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
add_definitions(-DSPDLOG_HEADER_ONLY)

# Find required packages
find_package(Boost REQUIRED COMPONENTS system)
find_package(CURL REQUIRED)
find_package(GTest REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)

# Find Arrow
find_package(Arrow REQUIRED)

# Find PkgConfig
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)

# Find GTest
find_package(GTest REQUIRED)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../data
    ${CMAKE_CURRENT_SOURCE_DIR}/../system
    ${CMAKE_CURRENT_SOURCE_DIR}/../strategy
    ${Arrow_INCLUDE_DIRS}
    ${PQXX_INCLUDE_DIRS}
    ${GTEST_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

# Link directories
link_directories(${PQXX_LIBRARY_DIRS})

# Add executables
add_executable(test_paper_trading
    ${CMAKE_CURRENT_SOURCE_DIR}/test_paper_trading_integration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_trend_strategy_paper_trade.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mock_ibkr_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/database_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/data_interface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../system/performance_analytics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../system/trading_dashboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../system/ibkr_interface.cpp
)

target_link_libraries(test_paper_trading
    PRIVATE
    GTest::GTest
    GTest::Main
    ${PQXX_LIBRARIES}
    ${Boost_LIBRARIES}
    ${CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    fmt::fmt-header-only
    Arrow::arrow_shared
    pthread
)

add_test(NAME test_paper_trading COMMAND test_paper_trading)

add_executable(test_db
    test_database_client.cpp
    ../data/data_interface.cpp
    ../data/database_client.cpp
    ../data/ohlcv_data_handler.cpp
)

add_executable(mock_trading
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/data_interface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/database_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/ohlcv_data_handler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../system/test_trend_strategy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../system/run_mock_trading.cpp)

add_executable(test_vol_regime
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/data_interface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/database_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/ohlcv_data_handler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../system/test_vol_regime.cpp)

add_executable(test_ibkr_paper_trader
    ${CMAKE_CURRENT_SOURCE_DIR}/test_ibkr_paper_trader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mock_ibkr_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../system/ibkr_interface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../strategy/trend_strategy.cpp
)

# Link libraries
target_link_libraries(test_db PRIVATE Arrow::arrow_shared ${PQXX_LIBRARIES} nlohmann_json::nlohmann_json)
target_link_libraries(mock_trading PRIVATE Arrow::arrow_shared ${PQXX_LIBRARIES} nlohmann_json::nlohmann_json)
target_link_libraries(test_vol_regime PRIVATE Arrow::arrow_shared ${PQXX_LIBRARIES} nlohmann_json::nlohmann_json)
target_link_libraries(test_ibkr_paper_trader
    PRIVATE
    GTest::GTest
    GTest::Main
    ${PQXX_LIBRARIES}
    ${Boost_LIBRARIES}
    ${CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    fmt::fmt-header-only
    pthread
    database_client
    ibkr_interface
)

# Create database client library
add_library(database_client
    STATIC
    ../database/database_client.cpp
)

target_link_libraries(database_client
    PRIVATE
    ${PQXX_LIBRARIES}
    spdlog::spdlog_header_only
    fmt::fmt-header-only
)

# Create IBKR interface library
add_library(ibkr_interface
    STATIC
    ../system/ibkr_interface.cpp
    ../system/real_ibkr_interface.cpp
)

target_link_libraries(ibkr_interface
    PRIVATE
    ${CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    fmt::fmt-header-only
)

# Set compile options
add_compile_options(-Wall -Wextra -O3)

# Set output directories to match existing build structure
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Register tests
add_test(NAME test_ibkr_paper_trader COMMAND test_ibkr_paper_trader)

# Optional: Windows-specific configurations (commented out but available if needed)
# if(WIN32)
#     set(CMAKE_TOOLCHAIN_FILE "C:/Users/rmathieu/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
#     target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
#     include_directories(
#         "C:/Program Files/PostgreSQL/16/include"
#         "C:/Users/rmathieu/vcpkg/installed/x64-windows/include"
#     )
#     link_directories(
#         "C:/Program Files/PostgreSQL/16/lib"
#         "C:/Users/rmathieu/vcpkg/installed/x64-windows/lib"
#     )
# endif()
