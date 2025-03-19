cmake_minimum_required(VERSION 3.17)
project(trade_ngin VERSION 1.0.0 LANGUAGES CXX)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
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
find_package(libpqxx CONFIG REQUIRED)

# Add source files for the library
set(TRADE_NGIN_SOURCES
    src/core/state_manager.cpp
    src/core/config_manager.cpp
    src/core/config_version.cpp
    src/core/config_base.cpp 
    src/core/logger.cpp
    src/data/postgres_database.cpp
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
    src/backtest/backtest_engine.cpp
    src/backtest/slippage_model.cpp
    src/strategy/base_strategy.cpp
    src/strategy/trend_following.cpp
)

# Add library target
add_library(trade_ngin SHARED ${TRADE_NGIN_SOURCES})

# Link against dependencies
target_link_libraries(trade_ngin
    PUBLIC
        nlohmann_json::nlohmann_json
        libpqxx::pqxx
        Arrow::arrow_shared
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

# Enable testing
enable_testing()

# Add tests subdirectory
add_subdirectory(tests)

# Add apps subdirectory
add_subdirectory(apps)