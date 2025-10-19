# ==========================================
# TRADE_NGIN - APPS CMAKE CONFIGURATION
# ==========================================

# Add subdirectories for applications
add_subdirectory(backtest)
add_subdirectory(strategies)

# Test program for database extensions
add_executable(test_database_extensions test_database_extensions.cpp)
target_link_libraries(test_database_extensions
    PRIVATE
        trade_ngin
        nlohmann_json::nlohmann_json
)
