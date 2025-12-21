# ============================================
# TRADE_NGIN - STRATEGIES CMAKE CONFIGURATION
# ============================================

set(LIVE_PORTFOLIO_SOURCES
    live_portfolio.cpp
)

# Create live portfolio executable (runs multiple strategies)
add_executable(live_portfolio ${LIVE_PORTFOLIO_SOURCES})

# Link against the trade_ngin library
target_link_libraries(live_portfolio
    PRIVATE
        trade_ngin
)

# Set output directory
set_target_properties(live_portfolio
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)