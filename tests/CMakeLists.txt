# Set test source files
set(TEST_SOURCES
    core/test_result.cpp
    core/test_state_manager.cpp
    core/test_logger.cpp
    core/test_config_manager.cpp
    core/test_config_version.cpp
    core/test_config_base.cpp
    core/test_holiday_lookback_extended_closure.cpp
    data/test_db_utils.cpp
    data/test_postgres_database.cpp
    data/test_database_pooling.cpp
    data/test_market_data_bus.cpp
    data/test_credential_store.cpp
    data/test_bar_close_uses_adjusted.cpp
    order/test_order_manager.cpp
    order/test_utils.cpp
    risk/test_risk_manager.cpp
    optimization/test_dynamic_optimizer.cpp
    backtesting/test_transaction_cost_analysis.cpp
    transaction_cost/test_adv_classifier_wireup.cpp
    transaction_cost/test_unknown_equity_costs_dispatch.cpp
    transaction_cost/test_equity_borrow_fees.cpp
    live/test_corp_actions_applier.cpp
    live/test_corp_actions_audit_log.cpp
    live/test_corp_actions_long_hold_total_return.cpp
    live/test_audit_log_cumulative_dividend.cpp
    live/test_live_metrics_includes_dividend_income.cpp
    live/test_corp_actions_ultrareview_fixes.cpp
    backtest/test_pnl_accounting_branch.cpp
    # backtesting/test_engine.cpp  # Deprecated - BacktestEngine replaced by BacktestCoordinator
    execution/test_execution_engine.cpp
    portfolio/test_portfolio_manager.cpp
    instruments/test_equity_instrument_holiday_check_wired.cpp
    instruments/test_exchange_json_wireup.cpp
    instruments/test_equity_margin_cash_regt.cpp
    instruments/test_equity_leverage_guardrail.cpp
    instruments/test_futures_margin_overload.cpp
    strategy/test_base_strategy.cpp
    strategy/test_trend_following.cpp
    strategy/test_mean_reversion_backtest.cpp
    strategy/test_equity_backtest_db_validation.cpp
    strategy/test_equity_pnl_accounting.cpp
    strategy/test_volatility_size_1_nan_guard.cpp
    strategy/test_stale_unrealized_pnl_zero_avg_price.cpp
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

target_compile_definitions(trade_ngin_tests PRIVATE TESTING)

# Link against Google Test and our library
target_link_libraries(trade_ngin_tests
    PRIVATE
    trade_ngin
    GTest::gtest
    GTest::gtest_main
)

# Set output directory
set_target_properties(trade_ngin_tests
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# Enable test discovery
include(GoogleTest)
# gtest_discover_tests(trade_ngin_tests)
add_test(NAME trade_ngin_tests COMMAND trade_ngin_tests)
