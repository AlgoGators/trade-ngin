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

# Options for dependency management
option(BUILD_TESTS "Build the test suite" ON)
option(ENABLE_DEPENDENCY_GRAPH "Generate dependency graph using Graphviz" OFF)
option(USE_VENDORED_LIBS "Use vendored libraries instead of system packages" OFF)
option(PIN_DEPENDENCY_VERSIONS "Use specific versions of dependencies" ON)

# Setup third party dependencies directory
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

# Add the cmake modules directory
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Include dependency management helpers
include(cmake/Dependencies.cmake)

# ==================================================================
# DEPENDENCIES
# ==================================================================

# Find required packages with version pinning if enabled
if(PIN_DEPENDENCY_VERSIONS)
    find_package(GTest 1.10.0 REQUIRED)
    find_package(nlohmann_json 3.10.0 REQUIRED)
    find_package(Arrow 6.0.0 REQUIRED)
    find_package(libpqxx 7.0.0 REQUIRED)
else()
    find_package(GTest REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(Arrow REQUIRED)
    find_package(libpqxx REQUIRED)
endif()

# Define component source files
# Core component
set(CORE_SOURCES
    src/core/state_manager.cpp
    src/core/config_manager.cpp
    src/core/config_version.cpp
    src/core/config_base.cpp 
    src/core/logger.cpp
    src/core/log_manager.cpp
    src/core/json_wrapper.cpp
)

# Data component
set(DATA_SOURCES
    src/data/postgres_database.cpp
    src/data/market_data_bus.cpp
    src/data/conversion_utils.cpp
    src/data/credential_store.cpp
    src/data/database_pooling.cpp
)

# Instruments component
set(INSTRUMENTS_SOURCES
    src/instruments/futures.cpp
    src/instruments/equity.cpp
    src/instruments/option.cpp
    src/instruments/instrument_registry.cpp
)

# Order component
set(ORDER_SOURCES
    src/order/order_manager.cpp
)

# Risk component
set(RISK_SOURCES
    src/risk/risk_manager.cpp
)

# Optimization component
set(OPTIMIZATION_SOURCES
    src/optimization/dynamic_optimizer.cpp
)

# Execution component
set(EXECUTION_SOURCES
    src/execution/execution_engine.cpp
)

# Portfolio component
set(PORTFOLIO_SOURCES
    src/portfolio/portfolio_manager.cpp
)

# Strategy component
set(STRATEGY_SOURCES
    src/strategy/base_strategy.cpp
    src/strategy/trend_following.cpp
)

# Backtest component
set(BACKTEST_SOURCES
    src/backtest/transaction_cost_analysis.cpp
    src/backtest/backtest_engine.cpp
    src/backtest/slippage_model.cpp
    src/backtest/backtest_config_manager.cpp
)

# ==================================================================
# LIBRARY TARGETS
# ==================================================================

# Build each component as a separate library
add_library(trade_ngin_core STATIC ${CORE_SOURCES})
add_library(trade_ngin_data STATIC ${DATA_SOURCES})
add_library(trade_ngin_instruments STATIC ${INSTRUMENTS_SOURCES})
add_library(trade_ngin_order STATIC ${ORDER_SOURCES})
add_library(trade_ngin_risk STATIC ${RISK_SOURCES})
add_library(trade_ngin_optimization STATIC ${OPTIMIZATION_SOURCES})
add_library(trade_ngin_execution STATIC ${EXECUTION_SOURCES})
add_library(trade_ngin_portfolio STATIC ${PORTFOLIO_SOURCES})
add_library(trade_ngin_strategy STATIC ${STRATEGY_SOURCES})
add_library(trade_ngin_backtest STATIC ${BACKTEST_SOURCES})

# Set public include directories
foreach(target 
    trade_ngin_core
    trade_ngin_data
    trade_ngin_instruments
    trade_ngin_order
    trade_ngin_risk
    trade_ngin_optimization
    trade_ngin_execution
    trade_ngin_portfolio
    trade_ngin_strategy
    trade_ngin_backtest
)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    )
endforeach()

# ==================================================================
# DEPENDENCIES BETWEEN COMPONENTS
# ==================================================================

# Set component dependencies
target_link_libraries(trade_ngin_core
    PUBLIC
        nlohmann_json::nlohmann_json
)

target_link_libraries(trade_ngin_data
    PUBLIC
        trade_ngin_core
        libpqxx::pqxx
)

target_link_libraries(trade_ngin_instruments
    PUBLIC
        trade_ngin_core
        trade_ngin_data
)

target_link_libraries(trade_ngin_order
    PUBLIC
        trade_ngin_core
        trade_ngin_data
        trade_ngin_instruments
)

target_link_libraries(trade_ngin_risk
    PUBLIC
        trade_ngin_core
        trade_ngin_data
        trade_ngin_instruments
    PRIVATE
        Arrow::arrow_shared
)

target_link_libraries(trade_ngin_optimization
    PUBLIC
        trade_ngin_core
        trade_ngin_data
    PRIVATE
        trade_ngin_risk
)

target_link_libraries(trade_ngin_execution
    PUBLIC
        trade_ngin_core
        trade_ngin_data
        trade_ngin_order
)

target_link_libraries(trade_ngin_portfolio
    PUBLIC
        trade_ngin_core
        trade_ngin_risk
        trade_ngin_optimization
    PRIVATE
        trade_ngin_data
        trade_ngin_order
)

target_link_libraries(trade_ngin_strategy
    PUBLIC
        trade_ngin_core
        trade_ngin_data
        trade_ngin_instruments
    PRIVATE
        trade_ngin_order
        trade_ngin_execution
)

target_link_libraries(trade_ngin_backtest
    PUBLIC
        trade_ngin_core
        trade_ngin_strategy
        trade_ngin_portfolio
    PRIVATE
        trade_ngin_data
        trade_ngin_instruments
        trade_ngin_risk
        trade_ngin_optimization
)

# Create the main trade_ngin library that pulls in all components
add_library(trade_ngin SHARED)
target_link_libraries(trade_ngin
    PUBLIC
        trade_ngin_core
        trade_ngin_data
        trade_ngin_instruments
        trade_ngin_order
        trade_ngin_risk
        trade_ngin_optimization
        trade_ngin_execution
        trade_ngin_portfolio
        trade_ngin_strategy
        trade_ngin_backtest
)

# Windows-specific settings
if(WIN32)
    set_target_properties(trade_ngin PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

# ==================================================================
# DEPENDENCY GRAPH VISUALIZATION
# ==================================================================

if(ENABLE_DEPENDENCY_GRAPH)
    # Check for graphviz/dot
    find_program(DOT_EXECUTABLE dot)
    if(DOT_EXECUTABLE)
        # Generate a GraphViz file with the dependency graph
        set(GRAPH_OUTPUT_DIR "${CMAKE_BINARY_DIR}/dependency_graphs")
        file(MAKE_DIRECTORY ${GRAPH_OUTPUT_DIR})
        
        # Generate GraphViz file
        set(GRAPHVIZ_GENERATE_PER_TARGET TRUE)
        set(GRAPHVIZ_GENERATE_DEPENDERS TRUE)
        set(GRAPHVIZ_IGNORE_TARGETS "^[^t]|test_")
        
        # Generate the dependency graph
        add_custom_target(dependency_graph ALL
            COMMAND ${CMAKE_COMMAND} 
                --graphviz=${GRAPH_OUTPUT_DIR}/trade_ngin_deps.dot 
                .
            COMMAND ${DOT_EXECUTABLE} 
                -Tpng ${GRAPH_OUTPUT_DIR}/trade_ngin_deps.dot 
                -o ${GRAPH_OUTPUT_DIR}/trade_ngin_deps.png
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating dependency graph in ${GRAPH_OUTPUT_DIR}"
        )
        
        message(STATUS "Dependency graph generation enabled")
    else()
        message(WARNING "Graphviz/dot not found, dependency graph generation disabled")
    endif()
endif()

# ==================================================================
# TESTING & APPS
# ==================================================================

# Enable testing if requested
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Add apps subdirectory
add_subdirectory(apps)

# ==================================================================
# INSTALLATION
# ==================================================================

# Install the libraries
install(TARGETS 
    trade_ngin
    trade_ngin_core
    trade_ngin_data
    trade_ngin_instruments
    trade_ngin_order
    trade_ngin_risk
    trade_ngin_optimization
    trade_ngin_execution
    trade_ngin_portfolio
    trade_ngin_strategy
    trade_ngin_backtest
    EXPORT trade_ngin-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

# Install headers
install(DIRECTORY include/ DESTINATION include)

# Export targets
install(EXPORT trade_ngin-targets
    FILE trade_ngin-targets.cmake
    NAMESPACE trade_ngin::
    DESTINATION lib/cmake/trade_ngin
)

# Create and install config file
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/trade_ngin-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/trade_ngin-config.cmake
    INSTALL_DESTINATION lib/cmake/trade_ngin
)

# Create and install version file
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/trade_ngin-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Install config and version files
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/trade_ngin-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/trade_ngin-config-version.cmake
    DESTINATION lib/cmake/trade_ngin
)