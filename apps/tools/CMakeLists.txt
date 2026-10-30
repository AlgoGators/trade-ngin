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

# Rolls step 2: the per-contract roll rule's held series per root, as CSV (read only; the reference
# check and the measures of report R2). Nothing in the engine calls the rule yet.
add_executable(contract_rolls_dump contract_rolls_dump.cpp)
target_link_libraries(contract_rolls_dump PRIVATE trade_ngin)
set_target_properties(contract_rolls_dump
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)
