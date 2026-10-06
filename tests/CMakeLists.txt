# Share test-only fixture helpers with the focused acceptance executables.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Set test source files
set(TEST_SOURCES
    apps/test_book_execution_phase.cpp
    apps/test_consumption_projection.cpp
    apps/test_live_portfolio_helpers.cpp
    apps/test_qt_book_tail.cpp
    apps/test_qt_desk_cycle.cpp
    apps/test_qt_empty_owner_accounting.cpp
    apps/test_qt_empty_owner_finalization.cpp
    apps/test_qt_empty_owner_report.cpp
    apps/test_qt_equity_cost_consumption.cpp
    apps/test_qt_equity_desk_cycle.cpp
    apps/test_qt_equity_position_transition.cpp
    apps/test_qt_equity_prior_continuation.cpp
    apps/test_qt_equity_prior_finalization.cpp
    apps/test_qt_prior_finalization.cpp
    apps/test_qt_processed_report.cpp
    apps/test_qt_report_quantity_projection.cpp
    apps/test_run_consumption.cpp
    core/test_email_delivery_policy.cpp
    core/test_logger_shutdown.cpp
    data/test_qt_desk_current_facts.cpp
    data/test_qt_desk_decision.cpp
    data/test_qt_desk_owner_scope.cpp
    data/test_qt_desk_read_set.cpp
    data/test_qt_empty_model_owner_publication.cpp
    data/test_qt_empty_model_owner_reference.cpp
    data/test_qt_equity_model_prior_binding.cpp
    data/test_risk_limits.cpp
    instruments/test_contract_multiplier.cpp
    live/test_csv_exporter_context.cpp
    live/test_csv_exporter_desk.cpp
    live/test_equity_sleeve_netting.cpp
    portfolio/test_component_book.cpp
    portfolio/test_component_optimizer_evaluator.cpp
    portfolio/test_component_quantity_normalization.cpp
    portfolio/test_component_risk_evaluator.cpp
    portfolio/test_pm_own_price_history.cpp
    portfolio/test_price_history_ordering.cpp
    portfolio/test_qt_component_cost.cpp
    portfolio/test_qt_empty_owner_evaluation.cpp
    portfolio/test_qt_equity_finalization_proof.cpp
    portfolio/test_qt_equity_proof.cpp
    portfolio/test_qt_eval_serialize.cpp
    portfolio/test_qt_evaluation.cpp
    portfolio/test_qt_offline_legacy_capacity.cpp
    portfolio/test_qt_wire.cpp
    portfolio/test_qt_workflow_parity.cpp
    portfolio/test_quantity_rule.cpp
    portfolio/test_risk_fail_closed.cpp
    portfolio/test_risk_loop_log_golden.cpp
    portfolio/test_risk_module_loop.cpp
    risk/test_risk_module_config.cpp
    risk/test_risk_modules.cpp
    transaction_cost/test_qt_equity_cost_integration.cpp
    core/test_decimal.cpp
    core/test_time_utils_parse.cpp
    core/test_result.cpp
    core/test_state_manager.cpp
    core/test_logger.cpp
    core/test_config_manager.cpp
    core/test_config_version.cpp
    core/test_config_base.cpp
    core/test_holiday_checker.cpp
    core/test_holiday_coverage_years.cpp
    core/test_logger_retention.cpp
    core/test_email_positions_table.cpp
    core/test_run_id_generator.cpp
    core/test_config_loader.cpp
    core/test_live_config_override.cpp
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
    live/corp_actions/test_class1_silent_row_announcement.cpp
    live/corp_actions/test_spinoff_zero_basis_parent.cpp
    live/corp_actions/test_spinoff_receipt.cpp
    live/corp_actions/test_spinoff_bar_routing.cpp
    live/corp_actions/test_spinoff_reverse_split.cpp
    live/corp_actions/test_spinoff_multi_child.cpp
    live/corp_actions/test_spinoff_retry_after_split.cpp
    live/corp_actions/test_spinoff_bar_warnings.cpp
    live/corp_actions/test_spinoff_refusal_bound.cpp
    live/corp_actions/test_spinoff_dividend_double_apply.cpp
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
    backtest/test_bt_equity_validation_config.cpp
    backtest/test_backtest_realized_flow.cpp
    backtest/test_backtest_unrealized_accounting.cpp
    execution/test_execution_engine.cpp
    portfolio/test_portfolio_manager.cpp
    core/test_equity_portfolio_namespace.cpp
    live/corp_actions/test_window.cpp
    live/test_effective_universe.cpp
    live/test_live_daily_cycle_carry_forward.cpp
    live/test_broker_frame_event_classes.cpp
    live/test_rolled_back_symbol_mark.cpp
    live/test_feed_freshness_universe.cpp
    live/test_feed_ingestion_assertion.cpp
    live/test_day_t_write_ordering.cpp
    data/test_current_holding_start_db.cpp
    data/test_current_holding_start_bound_db.cpp
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
    live/test_cost_model_feed.cpp
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
    strategy/test_signals_publication.cpp
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
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_BINARY_DIR}/include
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
gtest_discover_tests(trade_ngin_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME email_delivery_script_policy
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_email_delivery_policy.sh
)
add_test(
    NAME multi_book_scheduler
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_multi_book_scheduler.sh
)

# Explicitly-built only: the disposable PostgreSQL QT pipeline probe must not
# join the monolithic unit-test executable or its test discovery.
add_executable(qt_report_probe EXCLUDE_FROM_ALL integration/qt_report_probe.cpp)
target_include_directories(qt_report_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_report_probe PRIVATE trade_ngin)

add_executable(runtime_publication_probe EXCLUDE_FROM_ALL integration/runtime_publication_probe.cpp)
target_include_directories(runtime_publication_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(runtime_publication_probe PRIVATE trade_ngin)

add_executable(stream_storage_probe EXCLUDE_FROM_ALL integration/stream_storage_probe.cpp)
target_include_directories(stream_storage_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(stream_storage_probe PRIVATE trade_ngin)

add_executable(proposal_storage_probe EXCLUDE_FROM_ALL integration/proposal_storage_probe.cpp)
target_include_directories(proposal_storage_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(proposal_storage_probe PRIVATE trade_ngin)

# Pure decision-envelope admission. Retained in the full regression target too.
add_executable(qt_desk_decision_tests EXCLUDE_FROM_ALL data/test_qt_desk_decision.cpp)
add_executable(qt_desk_cycle_tests EXCLUDE_FROM_ALL apps/test_qt_desk_cycle.cpp)
add_executable(qt_book_tail_tests EXCLUDE_FROM_ALL apps/test_qt_book_tail.cpp)
target_include_directories(qt_book_tail_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_book_tail_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_book_tail_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
add_executable(book_execution_phase_tests EXCLUDE_FROM_ALL apps/test_book_execution_phase.cpp)
target_include_directories(book_execution_phase_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(book_execution_phase_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(book_execution_phase_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
add_executable(qt_prior_finalization_tests EXCLUDE_FROM_ALL apps/test_qt_prior_finalization.cpp)
target_include_directories(qt_prior_finalization_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_prior_finalization_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_prior_finalization_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
add_executable(qt_equity_cost_tests EXCLUDE_FROM_ALL
    transaction_cost/test_qt_equity_cost_integration.cpp
    transaction_cost/test_transaction_cost_manager.cpp
    transaction_cost/test_spread_model.cpp
    transaction_cost/test_impact_model.cpp)
add_executable(qt_equity_position_transition_tests EXCLUDE_FROM_ALL apps/test_qt_equity_position_transition.cpp)
add_executable(qt_equity_desk_cycle_tests EXCLUDE_FROM_ALL apps/test_qt_equity_desk_cycle.cpp)
add_executable(qt_equity_cost_consumption_tests EXCLUDE_FROM_ALL apps/test_qt_equity_cost_consumption.cpp)
add_executable(qt_logger_shutdown_tests EXCLUDE_FROM_ALL core/test_logger_shutdown.cpp core/test_logger.cpp)
add_executable(qt_equity_proof_tests EXCLUDE_FROM_ALL portfolio/test_qt_equity_proof.cpp portfolio/test_qt_offline_legacy_capacity.cpp portfolio/test_qt_equity_finalization_proof.cpp)
add_executable(qt_equity_prior_finalization_tests EXCLUDE_FROM_ALL apps/test_qt_equity_prior_finalization.cpp)
target_include_directories(qt_equity_prior_finalization_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_equity_prior_finalization_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_equity_prior_finalization_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
add_executable(qt_equity_prior_continuation_tests EXCLUDE_FROM_ALL apps/test_qt_equity_prior_continuation.cpp)
target_include_directories(qt_equity_prior_continuation_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_equity_prior_continuation_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_equity_prior_continuation_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
# Needs its owned network-none PostgreSQL (QT_N4_TEST_DSN); never part of trade_ngin_tests.
add_executable(qt_equity_prior_continuation_pg_tests EXCLUDE_FROM_ALL data/test_qt_equity_prior_continuation_pg.cpp)
target_include_directories(qt_equity_prior_continuation_pg_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_equity_prior_continuation_pg_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_equity_prior_continuation_pg_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
add_executable(backtest_position_replace_pg_tests EXCLUDE_FROM_ALL
    data/test_backtest_position_replace_pg.cpp)
target_include_directories(backtest_position_replace_pg_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(backtest_position_replace_pg_tests PRIVATE
    trade_ngin GTest::gtest GTest::gtest_main)

add_executable(investor_book_onboarding_pg_tests EXCLUDE_FROM_ALL
    data/test_investor_book_onboarding_pg.cpp)
target_include_directories(investor_book_onboarding_pg_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(investor_book_onboarding_pg_tests PRIVATE
    trade_ngin GTest::gtest GTest::gtest_main)
add_executable(equity_multi_live_pg_tests EXCLUDE_FROM_ALL
    data/test_equity_multi_live_pg.cpp)
target_include_directories(equity_multi_live_pg_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(equity_multi_live_pg_tests PRIVATE
    trade_ngin GTest::gtest GTest::gtest_main)
target_compile_definitions(equity_multi_live_pg_tests PRIVATE
    TRADE_NGIN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(equity_multi_live_pg_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
add_executable(equity_multi_backtest_pg_tests EXCLUDE_FROM_ALL
    data/test_equity_multi_backtest_pg.cpp)
target_include_directories(equity_multi_backtest_pg_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(equity_multi_backtest_pg_tests PRIVATE
    trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(equity_multi_backtest_pg_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
set_target_properties(backtest_position_replace_pg_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
add_executable(qt_equity_finalization_probe EXCLUDE_FROM_ALL integration/qt_equity_finalization_probe.cpp)
target_include_directories(qt_equity_finalization_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_equity_finalization_probe PRIVATE trade_ngin)
set_target_properties(qt_equity_finalization_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
target_include_directories(qt_equity_proof_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_equity_proof_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_equity_proof_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
target_include_directories(qt_logger_shutdown_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_logger_shutdown_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_logger_shutdown_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
target_include_directories(qt_equity_cost_consumption_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_equity_cost_consumption_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_equity_cost_consumption_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
add_executable(qt_consumption_projection_tests EXCLUDE_FROM_ALL apps/test_consumption_projection.cpp data/test_db_utils.cpp)
target_include_directories(qt_consumption_projection_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_consumption_projection_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_consumption_projection_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug")
target_include_directories(qt_equity_desk_cycle_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_equity_desk_cycle_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_equity_desk_cycle_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
target_include_directories(qt_equity_position_transition_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_equity_position_transition_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_equity_position_transition_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
target_include_directories(qt_equity_cost_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_equity_cost_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_equity_cost_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
target_include_directories(qt_desk_cycle_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_desk_cycle_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_desk_cycle_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
target_include_directories(qt_desk_decision_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_desk_decision_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)

add_executable(qt_desk_read_set_tests EXCLUDE_FROM_ALL data/test_qt_desk_read_set.cpp)
target_include_directories(qt_desk_read_set_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_desk_read_set_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_desk_read_set_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
set_target_properties(qt_desk_decision_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# No email transport tests: body generation and CSV rendering only.
add_executable(qt_desk_csv_tests EXCLUDE_FROM_ALL live/test_csv_exporter_desk.cpp)
target_include_directories(qt_desk_csv_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_desk_csv_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_desk_csv_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

add_executable(qt_report_projection_tests EXCLUDE_FROM_ALL
    apps/test_qt_report_quantity_projection.cpp
    apps/test_qt_processed_report.cpp live/test_csv_exporter.cpp)
target_include_directories(qt_report_projection_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_report_projection_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_report_projection_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
add_executable(qt_desk_current_facts_tests EXCLUDE_FROM_ALL data/test_qt_desk_current_facts.cpp)
target_include_directories(qt_desk_current_facts_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_desk_current_facts_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
add_executable(qt_desk_storage_probe EXCLUDE_FROM_ALL integration/qt_desk_storage_probe.cpp)
add_executable(qt_desk_upstream_probe EXCLUDE_FROM_ALL integration/qt_desk_upstream_probe.cpp)
target_include_directories(qt_desk_upstream_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_desk_upstream_probe PRIVATE trade_ngin)
set_target_properties(qt_desk_upstream_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
target_include_directories(qt_desk_storage_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_desk_storage_probe PRIVATE trade_ngin)
add_executable(qt_processed_report_probe EXCLUDE_FROM_ALL integration/qt_processed_report_probe.cpp)
target_include_directories(qt_processed_report_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_processed_report_probe PRIVATE trade_ngin ${CMAKE_DL_LIBS})
add_library(qt_no_delivery_guard SHARED EXCLUDE_FROM_ALL integration/qt_no_delivery_guard.cpp)
if(UNIX AND NOT APPLE)
    add_library(qt_model_capture_clock SHARED EXCLUDE_FROM_ALL integration/qt_model_capture_clock.cpp)
    target_link_libraries(qt_model_capture_clock PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(qt_model_capture_clock PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/Debug"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
        PREFIX "lib" OUTPUT_NAME "qt_model_capture_clock")
endif()
target_include_directories(qt_no_delivery_guard PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_no_delivery_guard PRIVATE trade_ngin)
add_dependencies(qt_processed_report_probe qt_no_delivery_guard)
set_target_properties(qt_desk_current_facts_tests qt_desk_storage_probe qt_processed_report_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

add_executable(qt_desk_csv_context_tests EXCLUDE_FROM_ALL live/test_csv_exporter_context.cpp)
target_include_directories(qt_desk_csv_context_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_desk_csv_context_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_desk_csv_context_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
add_executable(qt_empty_model_owner_tests EXCLUDE_FROM_ALL data/test_qt_empty_model_owner_publication.cpp)
target_include_directories(qt_empty_model_owner_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_empty_model_owner_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_empty_model_owner_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

add_executable(qt_empty_model_owner_reference_tests EXCLUDE_FROM_ALL data/test_qt_empty_model_owner_reference.cpp)
target_include_directories(qt_empty_model_owner_reference_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_empty_model_owner_reference_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_empty_model_owner_reference_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

add_executable(qt_empty_owner_evaluation_tests EXCLUDE_FROM_ALL portfolio/test_qt_empty_owner_evaluation.cpp)
target_include_directories(qt_empty_owner_evaluation_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_empty_owner_evaluation_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_empty_owner_evaluation_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

add_executable(qt_empty_owner_accounting_tests EXCLUDE_FROM_ALL apps/test_qt_empty_owner_accounting.cpp)
target_include_directories(qt_empty_owner_accounting_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_empty_owner_accounting_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_empty_owner_accounting_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

add_executable(qt_empty_owner_report_tests EXCLUDE_FROM_ALL apps/test_qt_empty_owner_report.cpp)
target_include_directories(qt_empty_owner_report_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_empty_owner_report_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_empty_owner_report_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

add_executable(qt_empty_owner_finalization_tests EXCLUDE_FROM_ALL apps/test_qt_empty_owner_finalization.cpp)
target_include_directories(qt_empty_owner_finalization_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_empty_owner_finalization_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_empty_owner_finalization_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

# PLAN14 r2: test-only seam for qt_desk_processor.cpp report(). The test compiles that production
# translation unit into this executable only; libtrade_ngin and every other target are unchanged.
add_executable(qt_desk_report_scope_tests EXCLUDE_FROM_ALL data/test_qt_desk_report_scope.cpp)
target_include_directories(qt_desk_report_scope_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_desk_report_scope_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_desk_report_scope_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

# A real bounded process protocol, including the operator export path.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME LiveConfigValidationToolProtocol
        COMMAND ${CMAKE_COMMAND} -E env
            "LIVE_CONFIG_VALIDATE=$<TARGET_FILE:live_config_validate>"
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core/test_live_config_override_tool.py)
    set_tests_properties(LiveConfigValidationToolProtocol PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
