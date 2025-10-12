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
    backtesting/test_engine.cpp
    execution/test_execution_engine.cpp
    portfolio/test_portfolio_manager.cpp
    strategy/test_base_strategy.cpp
    strategy/test_trend_following.cpp
)

# Create test executablecd
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
    Arrow::arrow_shared
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
