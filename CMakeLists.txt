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

# Canonical Release artifacts are packaged at a different absolute path than
# the CI build directory. Keep native dependency lookup relative to each ELF.
if(UNIX AND NOT APPLE)
    set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
endif()

# Bind every build to the checked-out revision. Release configuration is
# fail-closed: supplied identities may describe a Git-less package, but may
# never override or disagree with an available checkout, and a dirty checkout
# is never a production Release input.
set(TRADE_NGIN_GIT_FULL_RESULT 1)
set(TRADE_NGIN_GIT_SHORT_RESULT 1)
set(TRADE_NGIN_GIT_STATUS_RESULT 1)
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE TRADE_NGIN_DETECTED_FULL_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE TRADE_NGIN_GIT_FULL_RESULT)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE TRADE_NGIN_DETECTED_SHORT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE TRADE_NGIN_GIT_SHORT_RESULT)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=all
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE TRADE_NGIN_GIT_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE TRADE_NGIN_GIT_STATUS_RESULT)
endif()

if(DEFINED ENV{TRADE_NGIN_GIT_SHA} AND NOT "$ENV{TRADE_NGIN_GIT_SHA}" STREQUAL "")
    set(TRADE_NGIN_GIT_SHA "$ENV{TRADE_NGIN_GIT_SHA}")
elseif(TRADE_NGIN_GIT_SHORT_RESULT EQUAL 0)
    set(TRADE_NGIN_GIT_SHA "${TRADE_NGIN_DETECTED_SHORT_SHA}")
else()
    set(TRADE_NGIN_GIT_SHA "unknown")
endif()

if(DEFINED ENV{TRADE_NGIN_GIT_SHA_FULL} AND NOT "$ENV{TRADE_NGIN_GIT_SHA_FULL}" STREQUAL "")
    set(TRADE_NGIN_GIT_SHA_FULL "$ENV{TRADE_NGIN_GIT_SHA_FULL}")
elseif(TRADE_NGIN_GIT_FULL_RESULT EQUAL 0)
    set(TRADE_NGIN_GIT_SHA_FULL "${TRADE_NGIN_DETECTED_FULL_SHA}")
else()
    set(TRADE_NGIN_GIT_SHA_FULL "")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND
   TRADE_NGIN_GIT_SHORT_RESULT EQUAL 0 AND
   NOT TRADE_NGIN_GIT_SHA STREQUAL TRADE_NGIN_DETECTED_SHORT_SHA)
    message(FATAL_ERROR "release_identity_invalid: supplied short SHA disagrees with checkout")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND
   TRADE_NGIN_GIT_FULL_RESULT EQUAL 0 AND
   NOT TRADE_NGIN_GIT_SHA_FULL STREQUAL TRADE_NGIN_DETECTED_FULL_SHA)
    message(FATAL_ERROR "release_identity_invalid: supplied full SHA disagrees with checkout")
endif()

if(TRADE_NGIN_GIT_STATUS_RESULT EQUAL 0)
    if(TRADE_NGIN_GIT_STATUS STREQUAL "")
        set(TRADE_NGIN_GIT_DIRTY FALSE)
    else()
        set(TRADE_NGIN_GIT_DIRTY TRUE)
    endif()
elseif(DEFINED ENV{TRADE_NGIN_GIT_DIRTY})
    set(TRADE_NGIN_GIT_DIRTY "$ENV{TRADE_NGIN_GIT_DIRTY}")
else()
    set(TRADE_NGIN_GIT_DIRTY TRUE)
endif()

include(cmake/TradeNginReleaseIdentity.cmake)
trade_ngin_validate_release_identity("${CMAKE_BUILD_TYPE}" "${TRADE_NGIN_GIT_SHA}"
    "${TRADE_NGIN_GIT_SHA_FULL}" "${TRADE_NGIN_GIT_DIRTY}")

message(STATUS "Git SHA: ${TRADE_NGIN_GIT_SHA} (${TRADE_NGIN_GIT_SHA_FULL}), dirty=${TRADE_NGIN_GIT_DIRTY}")

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
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

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

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/git_version.hpp.in"
    "${CMAKE_BINARY_DIR}/include/trade_ngin/git_version.hpp"
    @ONLY
)

# Add source files for the library
set(TRADE_NGIN_SOURCES
    src/apps/book_execution_phase.cpp
    src/apps/book_tail.cpp
    src/apps/consumption_projection.cpp
    src/apps/equity_live_book.cpp
    src/apps/equity_model_action_frame.cpp
    src/apps/equity_model_action_source.cpp
    src/apps/equity_execution_consumption.cpp
    src/apps/equity_portfolio_consumption.cpp
    src/apps/equity_run_consumption.cpp
    src/apps/equity_strategy_consumption.cpp
    src/apps/equity_model_prior.cpp
    src/apps/equity_multi_live_runner.cpp
    src/apps/live_portfolio_helpers.cpp
    src/apps/qt_book_tail.cpp
    src/apps/qt_desk_cycle.cpp
    src/apps/qt_desk_diagnostics.cpp
    src/apps/qt_empty_owner_report.cpp
    src/apps/qt_equity_accounting_output.cpp
    src/apps/qt_equity_cost_consumption.cpp
    src/apps/qt_equity_desk_cycle.cpp
    src/apps/qt_equity_position_transition.cpp
    src/apps/qt_equity_prior_continuation.cpp
    src/apps/qt_equity_prior_finalization.cpp
    src/apps/qt_investor_report_loader.cpp
    src/apps/qt_prior_finalization.cpp
    src/apps/qt_processed_report.cpp
    src/apps/qt_report_quantity_projection.cpp
    src/core/state_manager.cpp
    src/core/config_manager.cpp
    src/core/config_version.cpp
    src/core/config_base.cpp
    src/core/config_loader.cpp
    src/core/logger.cpp
    src/core/qt_sha256.cpp
    src/core/email_sender.cpp
    src/core/chart_generator.cpp
    src/core/run_id_generator.cpp
    src/data/postgres_database.cpp
    src/data/postgres_database_extensions.cpp
    src/data/market_data_bus.cpp
    src/data/conversion_utils.cpp
    src/data/credential_store.cpp
    src/data/database_pooling.cpp
    src/data/postgres_qt_report.cpp
    src/data/postgres_runtime.cpp
    src/data/qt_desk_accounting.cpp
    src/data/qt_desk_current_facts.cpp
    src/data/qt_desk_market_capture.cpp
    src/data/qt_desk_processor.cpp
    src/data/qt_desk_publication.cpp
    src/data/qt_desk_read_set.cpp
    src/data/qt_desk_upstream.cpp
    src/data/qt_empty_model_owner_publication.cpp
    src/data/qt_empty_model_owner_reference.cpp
    src/data/qt_empty_model_owner_storage.cpp
    src/data/qt_equity_desk_accounting.cpp
    src/data/qt_equity_desk_finalization.cpp
    src/data/qt_equity_model_prior_binding.cpp
    src/data/qt_equity_prior_continuation_db.cpp
    src/data/qt_model_publication.cpp
    src/data/qt_seed_publication.cpp
    src/data/market_data_utils.cpp
    src/instruments/futures.cpp
    src/instruments/equity.cpp
    src/instruments/option.cpp
    src/instruments/instrument_registry.cpp
    src/instruments/contract_multiplier.cpp
    src/order/order_manager.cpp
    src/risk/risk_manager.cpp
    src/risk/basic_risk_modules.cpp
    src/risk/carver_risk_module.cpp
    src/risk/risk_module.cpp
    src/risk/risk_module_config.cpp
    src/optimization/dynamic_optimizer.cpp
    src/optimization/annualized_sample_covariance.cpp
    src/execution/execution_engine.cpp
    src/portfolio/portfolio_manager.cpp
    src/portfolio/component_book.cpp
    src/portfolio/component_optimizer_evaluator.cpp
    src/portfolio/component_quantity_normalization.cpp
    src/portfolio/component_risk_evaluator.cpp
    src/portfolio/qt_component_cost.cpp
    src/portfolio/qt_equity_proof.cpp
    src/portfolio/qt_eval_serialize.cpp
    src/portfolio/qt_eval_wire.cpp
    src/portfolio/qt_evaluation.cpp
    src/portfolio/qt_wire.cpp
    src/portfolio/quantity_rule.cpp
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
    src/live/equity_sleeve_netting.cpp
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
        OpenSSL::Crypto
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
        ${CMAKE_BINARY_DIR}/include
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

# This call intentionally lives in the parent scope after apps/tools has
# created every release binary.
trade_ngin_add_release_artifacts_target()

# Add benchmarks subdirectory (separate from tests; not in default test run)
add_subdirectory(benchmarks)

add_executable(equity_model_entry_unit
    tests/test_equity_model_compatibility.cpp
    tests/test_equity_model_v4.cpp
    tests/test_equity_model_v5.cpp
    tests/test_equity_model_numeric_history.cpp
    tests/test_equity_mean_reversion_consumption.cpp
    tests/test_equity_model_prior_arguments.cpp
    tests/apps/test_equity_multi_live_accounting.cpp
    tests/apps/test_equity_model_action_frame.cpp
    tests/apps/test_equity_action_run_consumption.cpp
    tests/test_equity_run_consumption.cpp)
target_link_libraries(equity_model_entry_unit PRIVATE
    trade_ngin GTest::gtest GTest::gtest_main)
target_include_directories(equity_model_entry_unit PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(equity_model_entry_unit PRIVATE TESTING USE_DIRECT_DB_CONNECTION)
set_target_properties(equity_model_entry_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

foreach(probe IN ITEMS
    equity_model_database_probe equity_model_v4_probe equity_event_utc_probe
    equity_utc_readers_probe equity_model_prior_probe equity_prior_publication_probe equity_model_config_probe
    equity_prior_binding_probe)
    add_executable(${probe} tests/${probe}.cpp)
    target_link_libraries(${probe} PRIVATE trade_ngin)
    target_compile_definitions(${probe} PRIVATE TESTING USE_DIRECT_DB_CONNECTION)
    set_target_properties(${probe} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
endforeach()


# Isolated pre-fix projector behavior against the same coherent library/headers.
add_executable(equity_run_consumption_baseline_red
    tests/test_equity_run_consumption.cpp
    tests/equity_model_baseline_v4/equity_run_consumption.cpp
    tests/equity_model_baseline_v4/equity_portfolio_consumption.cpp)
target_link_libraries(equity_run_consumption_baseline_red PRIVATE
    trade_ngin GTest::gtest GTest::gtest_main)
target_compile_definitions(equity_run_consumption_baseline_red PRIVATE TESTING USE_DIRECT_DB_CONNECTION)
