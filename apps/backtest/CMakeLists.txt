# ==========================================
# TRADE_NGIN - BACKTEST CMAKE CONFIGURATION
# ==========================================

# Backtest application source files
set(BT_TREND_SOURCES
    bt_trend.cpp
)

# Create trend following backtest executable
add_executable(bt_trend ${BT_TREND_SOURCES})

# Link against specific trade_ngin components
target_link_libraries(bt_trend
    PRIVATE
        trade_ngin_backtest  # Only link to what's needed
        trade_ngin_strategy
        trade_ngin_portfolio
        trade_ngin_core
)

# Set output directory
set_target_properties(bt_trend
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# Create directory for visualizer and config scripts
set(SCRIPT_DIR "${CMAKE_BINARY_DIR}/bin/scripts")
file(MAKE_DIRECTORY ${SCRIPT_DIR})

# Copy run_backtest.sh and visualize_results.sh to the binary directory
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/run_backtest.sh
    ${CMAKE_BINARY_DIR}/bin/run_backtest.sh
    COPYONLY
)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/visualize_results.sh)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/visualize_results.sh
        ${CMAKE_BINARY_DIR}/bin/visualize_results.sh
        COPYONLY
    )
endif()

# Create directory for sample configs
set(CONFIG_DIR "${CMAKE_BINARY_DIR}/bin/config")
file(MAKE_DIRECTORY ${CONFIG_DIR})

# Copy sample config if it exists
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../config/trend_strategy.json)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../config/trend_strategy.json
        ${CONFIG_DIR}/trend_strategy.json
        COPYONLY
    )
endif()

# Make shell scripts executable
if(UNIX)
    add_custom_command(TARGET bt_trend POST_BUILD
        COMMAND chmod +x ${CMAKE_BINARY_DIR}/bin/run_backtest.sh
        COMMAND chmod +x ${CMAKE_BINARY_DIR}/bin/visualize_results.sh
        COMMENT "Making shell scripts executable"
    )
endif()