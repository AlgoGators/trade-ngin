# ============================================
# TRADE_NGIN - STRATEGIES CMAKE CONFIGURATION
# ============================================

# The daily pipeline is shared: each executable is a thin main() selecting a
# portfolio, with live_portfolio_runner.cpp holding the implementation both use.
set(LIVE_PORTFOLIO_SOURCES
    live_portfolio.cpp
    live_portfolio_runner.cpp
)

set(LIVE_PORTFOLIO_CONSERVATIVE_SOURCES
    live_portfolio_conservative.cpp
    live_portfolio_runner.cpp
)

# Create live portfolio executable (runs multiple strategies)
add_executable(live_portfolio ${LIVE_PORTFOLIO_SOURCES})

# Create conservative live portfolio executable
add_executable(live_portfolio_conservative ${LIVE_PORTFOLIO_CONSERVATIVE_SOURCES})

# Link against the trade_ngin library
target_link_libraries(live_portfolio
    PRIVATE
        trade_ngin
)

target_link_libraries(live_portfolio_conservative
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

set_target_properties(live_portfolio_conservative
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)
# live_portfolio_runner.cpp includes "live_portfolio_runner.hpp" by plain name.
target_include_directories(live_portfolio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(live_portfolio_conservative PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
