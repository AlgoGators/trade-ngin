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

# Explicit Unix-socket accounting only; no daily runner or delivery composition.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PostgreSQL REQUIRED)
    add_executable(qt_desk_run qt_desk_run.cpp qt_desk_csv.cpp)
    target_link_libraries(qt_desk_run PRIVATE trade_ngin PostgreSQL::PostgreSQL)
    add_executable(qt_desk_prepare_sources qt_desk_prepare_sources.cpp)
    target_link_libraries(qt_desk_prepare_sources PRIVATE trade_ngin PostgreSQL::PostgreSQL)
    set_target_properties(qt_desk_prepare_sources PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
    )
    set_target_properties(qt_desk_run PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
    )
endif()

# Pure one-request evaluator: no database, runner, configuration or delivery entrypoint.
add_executable(qt_evaluator qt_evaluator.cpp)
target_link_libraries(qt_evaluator PRIVATE trade_ngin)
target_include_directories(qt_evaluator PRIVATE ${CMAKE_BINARY_DIR}/include)
set_target_properties(qt_evaluator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
)

# Explicit local packaging only; never part of a live runner or default build.
# The guarded build owner supplies an inspected dependency inventory. The
# stager independently verifies ELF closure and binds it to these exact targets.
set(QT_EVALUATOR_BUNDLE_INPUTS "${CMAKE_BINARY_DIR}/qt-evaluator-bundle-inputs.json" CACHE FILEPATH "Reviewed QT native dependency inventory JSON")
set(QT_EVALUATOR_BUNDLE_DIRECTORY "${CMAKE_BINARY_DIR}/qt-evaluator-bundle" CACHE PATH "New owned QT bundle destination")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    find_program(QT_EVALUATOR_PYTHON NAMES python3)
    if(QT_EVALUATOR_PYTHON)
        add_custom_target(qt_evaluator_bundle
            COMMAND "${QT_EVALUATOR_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/qt_evaluator_bundle.py"
                --contract "${CMAKE_CURRENT_SOURCE_DIR}/qt_evaluator_manifest.json"
                --inputs "${QT_EVALUATOR_BUNDLE_INPUTS}"
                --destination "${QT_EVALUATOR_BUNDLE_DIRECTORY}"
                --expected-executable "$<TARGET_FILE:qt_evaluator>"
                --expected-engine "$<TARGET_FILE:trade_ngin>"
                --expected-build "${TRADE_NGIN_GIT_SHA}"
                --expected-compiler-id "${CMAKE_CXX_COMPILER_ID}"
                --expected-compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
            DEPENDS qt_evaluator trade_ngin
            VERBATIM
            COMMENT "Stage a closed, pinned QT evaluator bundle in a new local directory"
        )
    endif()
endif()

add_executable(qt_empty_owner_publication_probe EXCLUDE_FROM_ALL qt_empty_owner_publication_probe.cpp)
target_link_libraries(qt_empty_owner_publication_probe PRIVATE trade_ngin ${CMAKE_DL_LIBS})
target_include_directories(qt_empty_owner_publication_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(qt_empty_owner_publication_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")

add_executable(qt_empty_owner_loader_probe EXCLUDE_FROM_ALL qt_empty_owner_loader_probe.cpp)
target_link_libraries(qt_empty_owner_loader_probe PRIVATE trade_ngin ${CMAKE_DL_LIBS})
target_include_directories(qt_empty_owner_loader_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(qt_empty_owner_loader_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
