cmake_minimum_required(VERSION 3.10)
project(trade_ngin)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable testing
enable_testing()

# Find required packages
find_package(GTest REQUIRED)

# Manually find libpqxx instead of using find_package
find_path(LIBPQXX_INCLUDE_DIR pqxx)
find_library(LIBPQXX_LIBRARY pqxx)

# Include directories
include_directories(${GTEST_INCLUDE_DIRS})
include_directories(${LIBPQXX_INCLUDE_DIR})

# Add all source files
add_library(trade_ngin_lib
    trade_ngin/system/portfolio.cpp
    trade_ngin/system/test_trend_strategy.cpp
    # Add other source files here
)

# Create test executables
# add_executable(test_strategy tests/test_strategy.cpp)
# add_executable(test_portfolio tests/test_portfolio.cpp)
# add_executable(test_integration tests/test_integration.cpp)

# Link libraries to tests


# Add tests to CTest
# add_test(NAME StrategyTest COMMAND test_strategy)
# add_test(NAME PortfolioTest COMMAND test_portfolio)
# add_test(NAME IntegrationTest COMMAND test_integration)
