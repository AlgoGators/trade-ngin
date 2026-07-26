# Set test source files
set(TEST_SOURCES
    core/test_decimal.cpp
    core/test_result.cpp
    core/test_state_manager.cpp
    core/test_state_manager_extended.cpp
    core/test_logger.cpp
    core/test_config_manager.cpp
    core/test_config_version.cpp
    core/test_config_version_extended.cpp
    core/test_config_base.cpp
    core/test_holiday_lookback_extended_closure.cpp
    core/test_config_base_extended.cpp
    core/test_run_id_generator.cpp
    core/test_config_loader.cpp
    core/test_config_manager_extended.cpp
    data/test_db_utils.cpp
    data/test_conversion_utils_safe_get.cpp
    data/test_utc_date_contract.cpp
    data/test_postgres_param_safety.cpp
    core/test_holiday_path_resolution.cpp
    instruments/test_equity_audit_status.cpp
    instruments/test_equity_market_hours_exchange.cpp
    data/test_postgres_database.cpp
    data/test_database_pooling.cpp
    data/test_database_pooling_extended.cpp
    data/test_market_data_bus.cpp
    data/test_credential_store.cpp
    data/test_bar_close_uses_adjusted.cpp
    data/test_credential_store_extended.cpp
    data/test_conversion_utils.cpp
    data/test_market_data_utils.cpp
    order/test_order_manager.cpp
    order/test_utils.cpp
    instruments/test_futures.cpp
    instruments/test_equity.cpp
    instruments/test_option.cpp
    instruments/test_instrument_registry.cpp
    risk/test_risk_manager.cpp
    risk/test_risk_manager_extended.cpp
    optimization/test_dynamic_optimizer.cpp
    optimization/test_dynamic_optimizer_extended.cpp
    backtesting/test_transaction_cost_analysis.cpp
    transaction_cost/test_adv_classifier_wireup.cpp
    transaction_cost/test_unknown_equity_costs_dispatch.cpp
    transaction_cost/test_equity_borrow_fees.cpp
    transaction_cost/test_spread_model.cpp
    transaction_cost/test_impact_model.cpp
    live/test_corp_actions_applier.cpp
    live/test_corp_actions_audit_log.cpp
    live/test_corp_actions_long_hold_total_return.cpp
    live/test_audit_log_cumulative_dividend.cpp
    live/test_live_metrics_includes_dividend_income.cpp
    live/test_corp_actions_ultrareview_fixes.cpp
    live/test_daily_pnl_identity.cpp
    backtest/test_pnl_accounting_branch.cpp
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
    execution/test_execution_engine_extended.cpp
    portfolio/test_portfolio_manager.cpp
    instruments/test_equity_instrument_holiday_check_wired.cpp
    instruments/test_exchange_json_wireup.cpp
    instruments/test_equity_margin_cash_regt.cpp
    instruments/test_equity_leverage_guardrail.cpp
    instruments/test_futures_margin_overload.cpp
    portfolio/test_portfolio_aggregation.cpp
    portfolio/test_portfolio_manager_extended.cpp
    portfolio/test_portfolio_manager_internals.cpp
    storage/test_backtest_results_manager.cpp
    storage/test_live_results_manager.cpp
    live/test_live_historical_metrics.cpp
    live/test_live_price_manager.cpp
    live/test_execution_manager.cpp
    live/test_margin_manager.cpp
    live/test_live_metrics_calculator.cpp
    live/test_live_pnl_manager.cpp
    live/test_live_data_loader.cpp
    live/test_csv_exporter.cpp
    strategy/test_base_strategy.cpp
    strategy/test_trend_following.cpp
    strategy/test_regime_detector.cpp
    strategy/test_trend_following_fast.cpp
    strategy/test_trend_following_slow.cpp
    strategy/test_mean_reversion_backtest.cpp
    strategy/test_equity_backtest_db_validation.cpp
    strategy/test_equity_pnl_accounting.cpp
    strategy/test_live_bt_signal_consistency.cpp
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