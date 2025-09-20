# ==========================================
# TRADE_NGIN - BACKTEST CMAKE CONFIGURATION
# ==========================================

# Backtest application source files
set(BT_TREND_SOURCES
    bt_trend.cpp
)

# Create trend following backtest executable
add_executable(bt_trend ${BT_TREND_SOURCES})

# Link against the trade_ngin library
target_link_libraries(bt_trend
    PRIVATE
        trade_ngin
        libpqxx::pqxx
)

# Set output directory
set_target_properties(bt_trend
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)
