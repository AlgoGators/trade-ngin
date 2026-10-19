# ============================================
# TRADE_NGIN - TOOLS
# ============================================

# HD D-17: AlgoLens asks the engine (desk service, CheckSettings) whether a desk settings change
# would be accepted by a live run before saving it.
add_executable(qt_settings_check qt_settings_check.cpp)
target_link_libraries(qt_settings_check PRIVATE trade_ngin)
set_target_properties(qt_settings_check
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)
