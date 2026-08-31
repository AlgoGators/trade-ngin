# benchmarks/CMakeLists.txt
# Benchmark harness for trade-ngin performance measurement.
# DO NOT include benchmarks in default test target — they are slow and
# should run separately for performance analysis.

# Main benchmark executable
add_executable(trade_ngin_bench
    benchmark_main.cpp
)

# Link against the main library and dependencies
target_link_libraries(trade_ngin_bench
    PRIVATE
        trade_ngin
        nlohmann_json::nlohmann_json
        Eigen3::Eigen
)

# Include benchmark headers
target_include_directories(trade_ngin_bench
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
)

# Set output directory (goes to bin/Release or bin/Debug)
set_target_properties(trade_ngin_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

# Mark as NOT part of default test target (benchmarks are slow)
# Users run manually: trade_ngin_bench [--json output.json]
