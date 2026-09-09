cmake_minimum_required(VERSION 3.17)
project(trade_ngin VERSION 1.0.0 LANGUAGES CXX)

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

# RPATH: executables and libtrade_ngin.dylib share an output directory, so
# resolve @rpath next to the loading binary. Without this, the build picks up
# whatever rpath leaks from the environment (e.g. an active conda env's lib
# dir) and binaries fail to find libtrade_ngin.dylib at launch.
set(CMAKE_BUILD_RPATH "@loader_path")
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)

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
find_package(NLopt REQUIRED)

# NEW-3 / M-17: scrub a stale absolute SDK path out of NLopt's imported target.
#
# Homebrew's NLopt ships NLoptLibraryDepends.cmake, which sets, verbatim:
#
#   set_target_properties(NLopt::nlopt PROPERTIES
#     INTERFACE_LINK_LIBRARIES "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk/usr/lib/libm.tbd")
#
# an absolute path baked in on the machine that built the bottle. When the local
# Command Line Tools are a different version that directory does not exist, and
# because it arrives through an INTERFACE property it propagates into the link
# line of every target that links trade_ngin -- 22 generated files on this
# checkout -- and every one of them fails to link.
#
# The workaround that grew around this was to reconfigure and then sed the
# generated build tree, by hand, after every reconfigure. That is what this
# replaces. NOTE for whoever chased the old memory note: the cause is NOT
# Homebrew's pkgconfig. It is this one line in NLopt's own exported target file
# (H8 in the E3 audit).
#
# The fix rewrites the imported target's interface: any element naming a
# nonexistent .sdk path is dropped and, where it was libm, replaced by plain "m"
# so the linker resolves it against whichever SDK is actually in use. Entries
# that exist on this machine are left exactly as they are, so on a host whose
# SDK does match, this is a no-op.
if(APPLE AND TARGET NLopt::nlopt)
    get_target_property(_nlopt_iface NLopt::nlopt INTERFACE_LINK_LIBRARIES)
    if(_nlopt_iface)
        set(_nlopt_fixed "")
        foreach(_lib IN LISTS _nlopt_iface)
            if(_lib MATCHES "\\.sdk/" AND NOT EXISTS "${_lib}")
                if(_lib MATCHES "libm\\.(tbd|dylib|a)$")
                    list(APPEND _nlopt_fixed "m")
                    message(STATUS
                        "NLopt: replaced stale SDK libm path with -lm (NEW-3): ${_lib}")
                else()
                    message(STATUS
                        "NLopt: dropped stale SDK path from imported target (NEW-3): ${_lib}")
                endif()
            else()
                list(APPEND _nlopt_fixed "${_lib}")
            endif()
        endforeach()
        set_target_properties(NLopt::nlopt PROPERTIES
                              INTERFACE_LINK_LIBRARIES "${_nlopt_fixed}")

        # Self-check. If a future NLopt bottle bakes in a stale absolute path of
        # some other shape, the scrub above will miss it and the only symptom
        # would be a link error in 22 unrelated targets with nothing pointing
        # back to here. Fail at configure time instead, naming the file to look
        # at.
        foreach(_lib IN LISTS _nlopt_fixed)
            if(IS_ABSOLUTE "${_lib}" AND NOT EXISTS "${_lib}")
                message(FATAL_ERROR
                    "NLopt::nlopt still carries a nonexistent absolute path after the NEW-3 "
                    "scrub: ${_lib}\n"
                    "It comes from NLoptLibraryDepends.cmake in the NLopt install prefix "
                    "(INTERFACE_LINK_LIBRARIES). Widen the scrub above to cover it.")
            endif()
        endforeach()
    endif()
    unset(_nlopt_iface)
    unset(_nlopt_fixed)
endif()

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
    src/data/market_data_utils.cpp
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
    src/strategy/mean_reversion.cpp
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
    src/live/execution_price_resolver.cpp
    src/live/margin_manager.cpp
    src/live/csv_exporter.cpp
    src/live/corporate_actions_applier.cpp
    src/live/corporate_actions_classification.cpp
    src/live/corporate_actions_lifecycle.cpp
    src/live/corporate_actions_audit_log.cpp
    src/live/broker_frame.cpp
    src/statistics/utils/statistics_utils.cpp
    src/statistics/transformers/normalizer.cpp
    src/statistics/transformers/pca.cpp
    src/statistics/tests/adf_test.cpp
    src/statistics/tests/kpss_test.cpp
    src/statistics/tests/johansen_test.cpp
    src/statistics/tests/engle_granger_test.cpp
    src/statistics/volatility/garch.cpp
    src/statistics/volatility/egarch.cpp
    src/statistics/volatility/gjr_garch.cpp
    src/statistics/volatility/dcc_garch.cpp
    src/statistics/tests/phillips_perron_test.cpp
    src/statistics/tests/variance_ratio_test.cpp
    src/statistics/state_estimation/kalman_filter.cpp
    src/statistics/state_estimation/hmm.cpp
    src/statistics/state_estimation/markov_switching.cpp
    src/statistics/state_estimation/extended_kalman_filter.cpp
    src/statistics/regression/ols_regression.cpp
    src/statistics/regression/ridge_regression.cpp
    src/statistics/regression/lasso_regression.cpp
    src/statistics/hurst_exponent.cpp
    src/statistics/preprocessing/outlier_handler.cpp
    src/statistics/preprocessing/missing_data_handler.cpp
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
        NLopt::nlopt
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

# Add benchmarks subdirectory (separate from tests; not in default test run)
add_subdirectory(benchmarks)