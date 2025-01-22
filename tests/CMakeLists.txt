# Enable testing with GTest
enable_testing()

# Add test files
add_executable(unit_tests
    unit/test_data_interface.cpp
    unit/test_portfolio.cpp
    unit/test_strategy.cpp
    unit/test_risk.cpp
    unit/test_optimization.cpp
)

target_link_libraries(unit_tests
    PRIVATE
    trade_ngin
    GTest::GTest
    GTest::Main
)

# Add tests to CTest
add_test(NAME unit_tests COMMAND unit_tests)

# Integration tests
add_executable(integration_tests
    integration/test_full_system.cpp
    integration/test_strategy_risk.cpp
    integration/test_portfolio_execution.cpp
)

target_link_libraries(integration_tests
    PRIVATE
    trade_ngin
    GTest::GTest
    GTest::Main
)

add_test(NAME integration_tests COMMAND integration_tests)