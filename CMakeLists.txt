cmake_minimum_required(VERSION 3.28)
project(trade_ngin VERSION 0.1.1 LANGUAGES CXX)

# Important for Python bindings
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Fix macOS SDK path to prevent recurring SDK errors
if(APPLE)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    message(STATUS "Using macOS SDK: ${CMAKE_OSX_SYSROOT}")
endif()

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable common compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wsign-compare)
endif()

# Output directories
# Multi-config generators (Visual Studio, Xcode) use the _DEBUG/_RELEASE variants.
# Single-config generators (Unix Makefiles, Ninja) use the unsuffixed variables;
# CMAKE_BUILD_TYPE is resolved at configure time so the path matches what the README expects.
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin/Debug")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin/Debug")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin/Debug")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}")
endif()

# Options
option(BUILD_TESTING "Build tests" OFF)

# Find packages
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(Eigen3 CONFIG REQUIRED)
find_package(NLopt CONFIG REQUIRED)
find_package(CURL REQUIRED)
add_subdirectory(extern/pybind11)

# Include code coverage module if enabled
include(cmake/CodeCoverage.cmake)

if(TARGET Arrow::arrow_static)
    set(ARROW_TARGET Arrow::arrow_static)
elseif(TARGET Arrow::arrow_shared)
    set(ARROW_TARGET Arrow::arrow_shared)
elseif(TARGET Arrow::arrow)
    set(ARROW_TARGET Arrow::arrow)
else()
    message(FATAL_ERROR "No Arrow target found")
endif()

# Add source files for the library
set(TRADE_NGIN_SOURCES
    src/core/state_manager.cpp
    src/core/config_manager.cpp
    src/core/config_version.cpp
    src/core/config_base.cpp
    src/core/config_loader.cpp
    src/core/logger.cpp
    src/core/email_sender.cpp
    src/core/chart_generator.cpp
    src/core/run_id_generator.cpp
    src/data/postgres_database.cpp
    src/data/postgres_database_extensions.cpp
    src/data/market_data_bus.cpp
    src/data/conversion_utils.cpp
    src/data/credential_store.cpp
    src/data/database_pooling.cpp
    src/instruments/futures.cpp
    src/instruments/equity.cpp
    src/instruments/option.cpp
    src/instruments/instrument_registry.cpp
    src/order/order_manager.cpp
    src/risk/risk_manager.cpp
    src/optimization/dynamic_optimizer.cpp
    src/execution/execution_engine.cpp
    src/portfolio/portfolio_manager.cpp
    src/backtest/transaction_cost_analysis.cpp
    src/backtest/backtest_pnl_manager.cpp
    src/backtest/backtest_price_manager.cpp
    src/backtest/backtest_metrics_calculator.cpp
    src/backtest/backtest_execution_manager.cpp
    src/backtest/backtest_data_loader.cpp
    src/backtest/backtest_portfolio_constraints.cpp
    src/backtest/backtest_coordinator.cpp
    src/backtest/backtest_csv_exporter.cpp
    src/backtest/slippage_model.cpp
    src/transaction_cost/asset_cost_config.cpp
    src/transaction_cost/spread_model.cpp
    src/transaction_cost/impact_model.cpp
    src/transaction_cost/transaction_cost_manager.cpp
    src/strategy/base_strategy.cpp
    src/strategy/trend_following.cpp
    src/strategy/trend_following_slow.cpp
    src/strategy/trend_following_fast.cpp
    src/strategy/regime_detector.cpp
    src/storage/results_manager_base.cpp
    src/storage/backtest_results_manager.cpp
    src/storage/live_results_manager.cpp
    src/live/live_data_loader.cpp
    src/live/live_historical_metrics.cpp
    src/live/live_metrics_calculator.cpp
    src/live/live_trading_coordinator.cpp
    src/live/live_price_manager.cpp
    src/live/live_pnl_manager.cpp
    src/live/execution_manager.cpp
    src/live/margin_manager.cpp
    src/live/csv_exporter.cpp
    src/statistics/utils/statistics_utils.cpp
    src/statistics/transformers/normalizer.cpp
    src/statistics/transformers/pca.cpp
    src/statistics/tests/adf_test.cpp
    src/statistics/tests/kpss_test.cpp
    src/statistics/tests/johansen_test.cpp
    src/statistics/tests/engle_granger_test.cpp
    src/statistics/volatility/garch.cpp
    src/statistics/state_estimation/kalman_filter.cpp
    src/statistics/state_estimation/hmm.cpp
    src/api/backtest_api.cpp
)

# Add library target
if(WIN32)
    add_library(trade_ngin STATIC ${TRADE_NGIN_SOURCES})
else()
    add_library(trade_ngin SHARED ${TRADE_NGIN_SOURCES})
endif()

install(TARGETS trade_ngin
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
    ARCHIVE DESTINATION lib
)
if(APPLE)
    set_target_properties(trade_ngin PROPERTIES
        INSTALL_RPATH "@loader_path"
    )
elseif(NOT WIN32)
    set_target_properties(trade_ngin PROPERTIES
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# Add test-specific compile definitions when building tests
if(BUILD_TESTING)
    target_compile_definitions(trade_ngin PRIVATE USE_DIRECT_DB_CONNECTION)
endif()

# Link against dependencies
target_link_libraries(trade_ngin
    PUBLIC
        nlohmann_json::nlohmann_json
        ${ARROW_TARGET}
        CURL::libcurl
        Eigen3::Eigen
        NLopt::nlopt
        libpqxx::pqxx
)

# Set up include directories for the library
target_include_directories(trade_ngin
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${Arrow_INCLUDE_DIRS}
)

# Apply coverage flags if enabled
if(ENABLE_COVERAGE)
    add_coverage_target(trade_ngin)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
    enable_testing()
endif()

# Add apps subdirectory
add_subdirectory(apps)

# Add python wrapper option
option(BUILD_PYTHON_WRAPPER "Build Python wrapper using pybind11" ON)
if (BUILD_PYTHON_WRAPPER)
    add_subdirectory(python)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
