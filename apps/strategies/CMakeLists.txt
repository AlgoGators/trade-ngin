# ============================================
# TRADE_NGIN - STRATEGIES CMAKE CONFIGURATION
# ============================================

set(LIVE_TREND_SOURCES
    live_trend.cpp
)

# Create live trend following executable
add_executable(live_trend ${LIVE_TREND_SOURCES})

# Link against the trade_ngin library
target_link_libraries(live_trend
    PRIVATE
        trade_ngin
)

# Set output directory
set_target_properties(live_trend
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)