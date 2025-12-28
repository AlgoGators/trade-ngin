# ==========================================
# TRADE_NGIN - BACKTEST CMAKE CONFIGURATION
# ==========================================

# Backtest application source files
set(BT_PORTFOLIO_SOURCES
    bt_portfolio.cpp
)

set(BT_PORTFOLIO_CONSERVATIVE_SOURCES
    bt_portfolio_conservative.cpp
)

# Create portfolio backtest executable (runs multiple strategies) - BASE PORTFOLIO
add_executable(bt_portfolio ${BT_PORTFOLIO_SOURCES})

# Create conservative portfolio backtest executable
add_executable(bt_portfolio_conservative ${BT_PORTFOLIO_CONSERVATIVE_SOURCES})

# Link against the trade_ngin library
target_link_libraries(bt_portfolio
    PRIVATE
        trade_ngin
)

target_link_libraries(bt_portfolio_conservative
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

set_target_properties(bt_portfolio_conservative
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)