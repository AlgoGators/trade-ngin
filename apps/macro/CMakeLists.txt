# ==========================================
# TRADE_NGIN - MACRO DFM RUNNER
# ==========================================

add_executable(macro_dfm_runner macro_dfm_runner.cpp)

target_link_libraries(macro_dfm_runner
    PRIVATE
        trade_ngin
)

set_target_properties(macro_dfm_runner
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# ==========================================
# TRADE_NGIN - MACRO MS-DFM RUNNER
# ==========================================

add_executable(macro_msdfm_runner macro_msdfm_runner.cpp)

target_link_libraries(macro_msdfm_runner
    PRIVATE
        trade_ngin
)

set_target_properties(macro_msdfm_runner
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# ==========================================
# TRADE_NGIN - MACRO REGIME PIPELINE RUNNER
# ==========================================

add_executable(macro_regime_pipeline_runner macro_regime_pipeline_runner.cpp)

target_link_libraries(macro_regime_pipeline_runner
    PRIVATE
        trade_ngin
)

set_target_properties(macro_regime_pipeline_runner
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# ==========================================
# TRADE_NGIN - BSTS REGIME DETECTOR
# ==========================================

add_executable(bsts_regime_detector
    ${CMAKE_SOURCE_DIR}/src/strategy/bsts_regime_detection_multiasset.cpp
)

target_link_libraries(bsts_regime_detector
    PRIVATE
        trade_ngin
)

set_target_properties(bsts_regime_detector
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# ==========================================
# TRADE_NGIN - MARKET REGIME PIPELINE RUNNER
# ==========================================

add_executable(market_regime_pipeline_runner market_regime_pipeline_runner.cpp)

target_link_libraries(market_regime_pipeline_runner
    PRIVATE
        trade_ngin
)

set_target_properties(market_regime_pipeline_runner
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)
