# Set test source files
set(TEST_SOURCES
    core/test_result.cpp
    core/test_state_manager.cpp
    core/test_logger.cpp
    core/test_config_manager.cpp
    core/test_config_version.cpp
    core/test_config_base.cpp
    data/test_db_utils.cpp
    data/test_postgres_database.cpp
    data/test_database_pooling.cpp
    data/test_market_data_bus.cpp
    data/test_credential_store.cpp
    order/test_order_manager.cpp
    order/test_utils.cpp
    risk/test_risk_manager.cpp
    optimization/test_dynamic_optimizer.cpp
    backtesting/test_transaction_cost_analysis.cpp
    backtesting/test_slippage_model.cpp
    backtesting/test_backtest_execution_manager.cpp
    backtesting/test_backtest_price_manager.cpp
    backtesting/test_backtest_portfolio_constraints.cpp
    backtesting/test_backtest_pnl_manager.cpp
    backtesting/test_backtest_metrics_calculator.cpp
    backtesting/test_backtest_data_loader.cpp
    backtesting/test_backtest_csv_exporter.cpp
    backtesting/test_backtest_coordinator.cpp
    # backtesting/test_engine.cpp  # Deprecated - BacktestEngine replaced by BacktestCoordinator
    execution/test_execution_engine.cpp
    portfolio/test_portfolio_manager.cpp
    strategy/test_base_strategy.cpp
    strategy/test_trend_following.cpp
    statistics/test_normalizer.cpp
    statistics/test_pca.cpp
    statistics/test_adf.cpp
    statistics/test_kpss.cpp
    statistics/test_johansen.cpp
    statistics/test_engle_granger.cpp
    statistics/test_garch.cpp
    statistics/test_kalman_filter.cpp
    statistics/test_hmm.cpp
    statistics/test_critical_values.cpp
    statistics/test_statistics_integration.cpp
    statistics/test_ols_regression.cpp
    statistics/test_ridge_regression.cpp
    statistics/test_lasso_regression.cpp
    statistics/test_egarch.cpp
    statistics/test_hurst_exponent.cpp
    statistics/test_markov_switching.cpp
    statistics/test_phillips_perron.cpp
    statistics/test_variance_ratio.cpp
    statistics/test_gjr_garch.cpp
    statistics/test_dcc_garch.cpp
    statistics/test_extended_kalman_filter.cpp
    statistics/test_outlier_handler.cpp
    statistics/test_missing_data_handler.cpp
    statistics/test_convergence_monitoring.cpp
)

# Create test executable
add_executable(trade_ngin_tests ${TEST_SOURCES})

# Set include directories for tests
target_include_directories(trade_ngin_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(trade_ngin_tests PRIVATE TESTING USE_DIRECT_DB_CONNECTION)

target_link_libraries(trade_ngin_tests
    PRIVATE
    trade_ngin
    GTest::gtest
    GTest::gtest_main
)

# Apply coverage flags if enabled
if(ENABLE_COVERAGE)
    add_coverage_target(trade_ngin_tests)
endif()

# Set output directory
set_target_properties(trade_ngin_tests
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# Enable test discovery
include(GoogleTest)
gtest_discover_tests(trade_ngin_tests)