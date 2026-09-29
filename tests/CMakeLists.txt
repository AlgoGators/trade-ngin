# Set test source files
set(TEST_SOURCES
    data/test_qt_desk_owner_scope.cpp
    data/test_qt_empty_model_owner_publication.cpp
    data/test_qt_empty_model_owner_reference.cpp
    portfolio/test_qt_empty_owner_evaluation.cpp
    apps/test_qt_empty_owner_accounting.cpp
    apps/test_qt_empty_owner_report.cpp
    apps/test_qt_empty_owner_finalization.cpp
    core/test_decimal.cpp
    core/test_result.cpp
    core/test_state_manager.cpp
    core/test_state_manager_extended.cpp
    core/test_logger.cpp
    core/test_logger_shutdown.cpp
    core/test_config_manager.cpp
    core/test_config_version.cpp
    core/test_config_version_extended.cpp
    core/test_config_base.cpp
    core/test_config_base_extended.cpp
    core/test_run_id_generator.cpp
    core/test_config_loader.cpp
    core/test_email_delivery_policy.cpp
    apps/test_live_portfolio_helpers.cpp
    apps/test_qt_report_quantity_projection.cpp
    apps/test_qt_processed_report.cpp
    apps/test_qt_desk_cycle.cpp
    apps/test_book_execution_phase.cpp
    apps/test_qt_prior_finalization.cpp
    apps/test_qt_equity_position_transition.cpp
    apps/test_qt_equity_desk_cycle.cpp
    apps/test_qt_book_tail.cpp
    apps/test_qt_equity_prior_finalization.cpp
    apps/test_qt_equity_cost_consumption.cpp
    portfolio/test_qt_equity_proof.cpp
    portfolio/test_qt_offline_legacy_capacity.cpp
    portfolio/test_qt_equity_finalization_proof.cpp
    apps/test_run_consumption.cpp
    apps/test_consumption_projection.cpp
    core/test_config_manager_extended.cpp
    data/test_db_utils.cpp
    data/test_qt_desk_decision.cpp
    data/test_qt_desk_read_set.cpp
    data/test_qt_desk_current_facts.cpp
    data/test_postgres_database.cpp
    data/test_risk_limits.cpp
    data/test_database_pooling.cpp
    data/test_database_pooling_extended.cpp
    data/test_market_data_bus.cpp
    data/test_credential_store.cpp
    data/test_credential_store_extended.cpp
    data/test_conversion_utils.cpp
    order/test_order_manager.cpp
    order/test_utils.cpp
    instruments/test_futures.cpp
    instruments/test_equity.cpp
    instruments/test_option.cpp
    instruments/test_instrument_registry.cpp
    instruments/test_contract_multiplier.cpp
    risk/test_risk_manager.cpp
    risk/test_risk_manager_extended.cpp
    optimization/test_dynamic_optimizer.cpp
    optimization/test_dynamic_optimizer_extended.cpp
    backtesting/test_transaction_cost_analysis.cpp
    transaction_cost/test_spread_model.cpp
    transaction_cost/test_impact_model.cpp
    transaction_cost/test_transaction_cost_manager.cpp
    transaction_cost/test_qt_equity_cost_integration.cpp
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
    portfolio/test_portfolio_aggregation.cpp
    portfolio/test_portfolio_manager_extended.cpp
    portfolio/test_portfolio_manager_internals.cpp
    portfolio/test_component_book.cpp
    portfolio/test_qt_wire.cpp
    portfolio/test_qt_workflow_parity.cpp
    portfolio/test_qt_eval_serialize.cpp
    portfolio/test_qt_component_cost.cpp
    portfolio/test_qt_evaluation.cpp
    portfolio/test_component_risk_evaluator.cpp
    portfolio/test_component_optimizer_evaluator.cpp
    portfolio/test_quantity_rule.cpp
    portfolio/test_component_quantity_normalization.cpp
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
    live/test_csv_exporter_desk.cpp
    live/test_csv_exporter_context.cpp
    strategy/test_base_strategy.cpp
    strategy/test_trend_following.cpp
    strategy/test_regime_detector.cpp
    strategy/test_trend_following_fast.cpp
    strategy/test_trend_following_slow.cpp
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
# A bounded pure QT target keeps contract iterations independent of database,
# delivery and strategy-runner test entrypoints. The same files remain in the
# complete regression target above.
add_executable(qt_workflow_tests EXCLUDE_FROM_ALL
    data/test_qt_desk_owner_scope.cpp
    portfolio/test_qt_wire.cpp
    portfolio/test_qt_workflow_parity.cpp
    portfolio/test_qt_eval_serialize.cpp
    portfolio/test_qt_component_cost.cpp
    portfolio/test_qt_evaluation.cpp
)
target_include_directories(qt_workflow_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(qt_workflow_tests PRIVATE trade_ngin GTest::gtest GTest::gtest_main)
set_target_properties(qt_workflow_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# Enable test discovery for the complete suite.
include(GoogleTest)
gtest_discover_tests(trade_ngin_tests)

add_test(
    NAME email_delivery_script_policy
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_email_delivery_policy.sh
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
