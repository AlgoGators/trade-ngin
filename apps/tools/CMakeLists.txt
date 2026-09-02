# ==========================================
# TRADE_NGIN - TOOLS CMAKE CONFIGURATION
# ==========================================
# Standalone operational tools that aren't part of the daily live/backtest
# pipelines -- e.g. benchmark_replay (ADR-005 5.3).

set(BENCHMARK_REPLAY_SOURCES
    benchmark_replay.cpp
)

add_executable(benchmark_replay ${BENCHMARK_REPLAY_SOURCES})

target_link_libraries(benchmark_replay
    PRIVATE
        trade_ngin
)

set_target_properties(benchmark_replay
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# Needed for trade_ngin/git_version.hpp, generated at configure time (see
# root CMakeLists.txt) -- same requirement as apps/strategies.
target_include_directories(benchmark_replay PRIVATE ${CMAKE_BINARY_DIR}/include)
