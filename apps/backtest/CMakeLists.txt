# ==========================================
# TRADE_NGIN - BACKTEST CMAKE CONFIGURATION
# ==========================================

# Backtest application source files
set(BT_PORTFOLIO_SOURCES
    bt_portfolio.cpp
)

# Create portfolio backtest executable (runs multiple strategies)
add_executable(bt_portfolio ${BT_PORTFOLIO_SOURCES})

# Link against the trade_ngin library
target_link_libraries(bt_portfolio
    PRIVATE
        trade_ngin
)

# Set output directory
set_target_properties(bt_portfolio
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)