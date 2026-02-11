cmake_minimum_required(VERSION 3.17)
project(trade_ngin VERSION 1.0.0 LANGUAGES CXX)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

# Find packages
#set(GTest_DIR "C:/vcpkg/installed/x64-windows/share/gtest")
find_package(GTest CONFIG REQUIRED)
#set(nlohmann_json_DIR "C:/vcpkg/installed/x64-windows/share/nlohmann_json")
find_package(nlohmann_json CONFIG REQUIRED)
#set(Arrow_DIR "C:/vcpkg/installed/x64-windows/share/arrow")
find_package(Arrow REQUIRED)
#set(libpqxx_DIR "C:/vcpkg/installed/x64-windows/share/libpqxx")
# find_package(libpqxx CONFIG REQUIRED)
find_package(Eigen3 REQUIRED)

# Try to find libpqxx via pkg-config (works with Homebrew)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)

# Find curl for email functionality
find_package(CURL REQUIRED)

add_library(libpqxx::pqxx UNKNOWN IMPORTED)
set_target_properties(libpqxx::pqxx PROPERTIES
    IMPORTED_LOCATION "${PQXX_LINK_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${PQXX_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${PQXX_LIBRARIES}"
)

link_directories(${PQXX_LIBRARY_DIRS})

link_directories(/opt/homebrew/lib)

# Include code coverage module if enabled
include(cmake/CodeCoverage.cmake)

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
    src/live/live_metrics_calculator.cpp
    src/live/live_trading_coordinator.cpp
    src/live/live_price_manager.cpp
    src/live/live_pnl_manager.cpp
    src/live/execution_manager.cpp
    src/live/margin_manager.cpp
    src/live/csv_exporter.cpp
    src/statistics/statistics_tools.cpp
)

# Add library target
add_library(trade_ngin SHARED ${TRADE_NGIN_SOURCES})

 # Add test-specific compile definitions when building tests
if(BUILD_TESTING)
    target_compile_definitions(trade_ngin PRIVATE USE_DIRECT_DB_CONNECTION)
endif()

# Link against dependencies
target_link_libraries(trade_ngin
    PUBLIC
        nlohmann_json::nlohmann_json
        libpqxx::pqxx
        Arrow::arrow_shared
        CURL::libcurl
        Eigen3::Eigen
)

# Windows-specific settings
if(WIN32)
    set_target_properties(trade_ngin PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

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

# Enable testing
enable_testing()

# Add tests subdirectory
add_subdirectory(tests)

# Add apps subdirectory
add_subdirectory(apps)