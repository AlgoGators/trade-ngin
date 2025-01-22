# Find GTest package
find_package(GTest REQUIRED)

# Include directories
include_directories(${GTEST_INCLUDE_DIRS})

# Add test executables
add_executable(test_strategy 
    test_strategy.cpp
    ${CMAKE_SOURCE_DIR}/trade_ngin/system/test_trend_strategy.cpp
    ${CMAKE_SOURCE_DIR}/trade_ngin/data/database_client.cpp
    ${CMAKE_SOURCE_DIR}/trade_ngin/data/ohlcv_data_handler.cpp
)
add_executable(test_portfolio 
    test_portfolio.cpp
    ${CMAKE_SOURCE_DIR}/trade_ngin/system/portfolio.cpp
)
add_executable(test_integration 
    test_integration.cpp
    ${CMAKE_SOURCE_DIR}/trade_ngin/data/data_interface.cpp
    ${CMAKE_SOURCE_DIR}/trade_ngin/data/database_client.cpp
    ${CMAKE_SOURCE_DIR}/trade_ngin/system/portfolio.cpp
    ${CMAKE_SOURCE_DIR}/trade_ngin/system/test_trend_strategy.cpp
    ${CMAKE_SOURCE_DIR}/trade_ngin/data/ohlcv_data_handler.cpp
)

# Link libraries for test_strategy
target_link_libraries(test_strategy
    PRIVATE
    ${GTEST_LIBRARIES}
    Arrow::arrow_shared
    ${PQXX_LIBRARIES}
    nlohmann_json::nlohmann_json
    pthread
)

# Link libraries for test_portfolio
target_link_libraries(test_portfolio
    PRIVATE
    ${GTEST_LIBRARIES}
    Arrow::arrow_shared
    ${PQXX_LIBRARIES}
    nlohmann_json::nlohmann_json
    pthread
)

# Link libraries for test_integration
target_link_libraries(test_integration
    PRIVATE
    ${GTEST_LIBRARIES}
    Arrow::arrow_shared
    ${PQXX_LIBRARIES}
    nlohmann_json::nlohmann_json
    pthread
)

# Add tests to CTest
add_test(NAME test_strategy COMMAND test_strategy)
add_test(NAME test_portfolio COMMAND test_portfolio)
add_test(NAME test_integration COMMAND test_integration)

# Set test properties
set_tests_properties(test_strategy test_portfolio test_integration
    PROPERTIES
    ENVIRONMENT "POSTGRES_CONNECTION=postgresql://localhost:5432/trade_ngin") 