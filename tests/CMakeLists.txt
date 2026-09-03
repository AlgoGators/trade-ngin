# Set test source files
set(TEST_SOURCES
    core/test_decimal.cpp
    core/test_time_utils_parse.cpp
    core/test_result.cpp
    core/test_state_manager.cpp
    core/test_logger.cpp
    core/test_config_manager.cpp
    core/test_config_version.cpp
    core/test_config_base.cpp
    core/test_holiday_checker.cpp
    core/test_email_positions_table.cpp
    core/test_run_id_generator.cpp
    core/test_config_loader.cpp
    data/test_db_utils.cpp
    data/test_utc_date_contract.cpp
    data/test_postgres_param_safety.cpp
    instruments/test_equity_market_hours_exchange.cpp
    data/test_postgres_database.cpp
    data/test_database_pooling.cpp
    data/test_db_transaction_atomicity.cpp
    data/test_delete_stale_executions_scope_db.cpp
    data/test_get_trading_days_scope_db.cpp
    data/test_market_data_bus.cpp
    data/test_credential_store.cpp
    data/test_bar_close_uses_adjusted.cpp
    data/test_conversion_utils.cpp
    data/test_market_data_utils.cpp
    order/test_order_manager.cpp
    order/test_utils.cpp
    instruments/test_futures.cpp
    instruments/test_equity.cpp
    instruments/test_option.cpp
    instruments/test_instrument_registry.cpp
    risk/test_risk_manager.cpp
    optimization/test_dynamic_optimizer.cpp
    backtest/test_transaction_cost_analysis.cpp
    transaction_cost/test_transaction_cost_manager.cpp
    transaction_cost/test_spread_model.cpp
    transaction_cost/test_impact_model.cpp
    live/corp_actions/test_applier.cpp
    live/corp_actions/test_effect_classes.cpp
    live/corp_actions/test_frame_horizon.cpp
    live/corp_actions/test_ex_date_eligibility.cpp
    live/corp_actions/test_rename_eras.cpp
    live/corp_actions/test_denominator_frame.cpp
    live/corp_actions/test_denominator_range_db.cpp
    live/corp_actions/test_corp_action_query_bounds_db.cpp
    live/corp_actions/test_audit_log.cpp
    live/corp_actions/test_income.cpp
    live/corp_actions/test_broker_frame.cpp
    live/corp_actions/test_broker_frame_db.cpp
    live/corp_actions/test_spinoff_receipt.cpp
    live/test_live_metrics_includes_dividend_income.cpp
    live/test_daily_pnl_identity.cpp
    live/test_trading_days_anchor.cpp
    backtest/test_slippage_model.cpp
    backtest/test_backtest_execution_manager.cpp
    backtest/test_backtest_price_manager.cpp
    backtest/test_backtest_portfolio_constraints.cpp
    backtest/test_backtest_pnl_manager.cpp
    backtest/test_backtest_metrics_calculator.cpp
    backtest/test_backtest_data_loader.cpp
    backtest/test_backtest_csv_exporter.cpp
    backtest/test_backtest_coordinator.cpp
    backtest/test_backtest_unrealized_accounting.cpp
    execution/test_execution_engine.cpp
    portfolio/test_portfolio_manager.cpp
    core/test_equity_portfolio_namespace.cpp
    live/corp_actions/test_window.cpp
    live/test_effective_universe.cpp
    live/test_live_daily_cycle_carry_forward.cpp
    data/test_current_holding_start_db.cpp
    instruments/test_equity_margin_cash_regt.cpp
    instruments/test_futures_margin_overload.cpp
    portfolio/test_portfolio_aggregation.cpp
    portfolio/test_portfolio_manager_internals.cpp
    portfolio/test_fractional_position_convergence.cpp
    storage/test_backtest_results_manager.cpp
    storage/test_live_results_manager.cpp
    storage/test_live_results_key_roundtrip_db.cpp
    live/test_live_historical_metrics.cpp
    live/test_live_historical_metrics_columns.cpp
    live/test_live_price_manager.cpp
    live/test_live_trading_coordinator_keying.cpp
    live/test_execution_manager.cpp
    live/test_execution_price_integrity.cpp
    live/test_margin_manager.cpp
    live/test_live_metrics_calculator.cpp
    live/test_live_pnl_manager.cpp
    live/test_finalize_unrealized_policy.cpp
    live/test_finalize_realized_policy.cpp
    live/test_realized_row_retention.cpp
    live/test_unrealized_cost_basis.cpp
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
    strategy/test_live_daily_cycle_seeding.cpp
    strategy/test_penny_stock_lifecycle.cpp
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