# ============================================
# TRADE_NGIN - STRATEGIES CMAKE CONFIGURATION
# ============================================

set(TREND_FOLLOWING_SOURCES
    trend_following.cpp
)

# Create strategy optimizer executable
add_executable(trend_following ${TREND_FOLLOWING_SOURCES})

# Link against the trade_ngin library
target_link_libraries(trend_following
    PRIVATE
        trade_ngin
)

# Set output directory
set_target_properties(trend_following
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)