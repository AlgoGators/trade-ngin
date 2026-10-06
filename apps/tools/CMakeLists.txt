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

include("${CMAKE_CURRENT_LIST_DIR}/qt_desk_worker.cmake")

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
        add_custom_command(OUTPUT "${QT_EVALUATOR_BUNDLE_INPUTS}"
            COMMAND "${QT_EVALUATOR_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/qt_evaluator_bundle_inventory.py"
                --evaluator "$<TARGET_FILE:qt_evaluator>"
                --engine "$<TARGET_FILE:trade_ngin>"
                --build-id "${TRADE_NGIN_GIT_SHA}"
                --compiler-id "${CMAKE_CXX_COMPILER_ID}"
                --compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
                --output "${QT_EVALUATOR_BUNDLE_INPUTS}"
            DEPENDS qt_evaluator trade_ngin
            VERBATIM
            COMMENT "Inspect the exact built QT evaluator dependency closure")
        add_custom_command(OUTPUT "${QT_EVALUATOR_BUNDLE_DIRECTORY}/qt_evaluator_manifest.json"
            COMMAND "${QT_EVALUATOR_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/qt_evaluator_bundle.py"
                --contract "${CMAKE_CURRENT_SOURCE_DIR}/qt_evaluator_manifest.json"
                --inputs "${QT_EVALUATOR_BUNDLE_INPUTS}"
                --destination "${QT_EVALUATOR_BUNDLE_DIRECTORY}"
                --expected-executable "$<TARGET_FILE:qt_evaluator>"
                --expected-engine "$<TARGET_FILE:trade_ngin>"
                --expected-build "${TRADE_NGIN_GIT_SHA}"
                --expected-compiler-id "${CMAKE_CXX_COMPILER_ID}"
                --expected-compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
            DEPENDS qt_evaluator trade_ngin "${QT_EVALUATOR_BUNDLE_INPUTS}"
            VERBATIM
            COMMENT "Stage a closed, pinned QT evaluator bundle in a new local directory"
        )
        add_custom_target(qt_evaluator_bundle
            DEPENDS "${QT_EVALUATOR_BUNDLE_DIRECTORY}/qt_evaluator_manifest.json")
    endif()
endif()

# The worker is a mandatory Linux production artifact. Refer to the target
# directly: normal CMake variables are directory-scoped, while targets are
# visible from the parent scope that creates the release graph.
set(TRADE_NGIN_TOOLCHAIN_IMAGE_DIGEST "" CACHE STRING "Immutable sha256 digest of the Release build image")
set(TRADE_NGIN_IMAGE_DIGEST "" CACHE STRING "Immutable sha256 digest of the pushed runtime image")
set(TRADE_NGIN_RELEASE_MANIFEST "${CMAKE_BINARY_DIR}/release-artifacts.json" CACHE FILEPATH "release-artifacts/v1 output")

function(trade_ngin_add_release_artifacts_target)
    if(NOT QT_EVALUATOR_PYTHON OR NOT TARGET qt_evaluator_bundle)
        return()
    endif()
    if(NOT TARGET qt_desk_worker)
        message(FATAL_ERROR "release_artifacts_invalid: qt_desk_worker target is required")
    endif()
    set(release_dependencies
        qt_evaluator_bundle trade_ngin qt_evaluator qt_desk_prepare_sources qt_desk_run
        live_portfolio live_portfolio_conservative live_equity_mr
        qt_desk_worker)
    set(inputs "${CMAKE_BINARY_DIR}/release-artifacts-inputs-$<CONFIG>.json")
    file(GENERATE OUTPUT "${inputs}" CONTENT
        "{\"source\":{\"git_sha_full\":\"${TRADE_NGIN_GIT_SHA_FULL}\",\"git_sha_short\":\"${TRADE_NGIN_GIT_SHA}\",\"dirty\":false},\"build\":{\"build_type\":\"${CMAKE_BUILD_TYPE}\",\"compiler\":{\"id\":\"${CMAKE_CXX_COMPILER_ID}\",\"version\":\"${CMAKE_CXX_COMPILER_VERSION}\"},\"cxx_standard\":\"20\",\"toolchain_image_digest\":\"${TRADE_NGIN_TOOLCHAIN_IMAGE_DIGEST}\",\"cmake_inputs\":[\"CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}\",\"CMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}\",\"CMAKE_CXX_STANDARD=20\",\"NLopt_DIR=${NLopt_DIR}\"]},\"image_digest\":\"${TRADE_NGIN_IMAGE_DIGEST}\",\"expected_bundle_sha256\":\"auto\",\"artifacts\":{\"libtrade_ngin.so\":{\"source\":\"$<TARGET_FILE:trade_ngin>\",\"install_path\":\"bin/Release/libtrade_ngin.so\",\"kind\":\"engine\"},\"live_equity_mr\":{\"source\":\"$<TARGET_FILE:live_equity_mr>\",\"install_path\":\"bin/Release/live_equity_mr\",\"kind\":\"system_publisher\"},\"live_portfolio\":{\"source\":\"$<TARGET_FILE:live_portfolio>\",\"install_path\":\"bin/Release/live_portfolio\",\"kind\":\"system_publisher\"},\"live_portfolio_conservative\":{\"source\":\"$<TARGET_FILE:live_portfolio_conservative>\",\"install_path\":\"bin/Release/live_portfolio_conservative\",\"kind\":\"system_publisher\"},\"qt_desk_prepare_sources\":{\"source\":\"$<TARGET_FILE:qt_desk_prepare_sources>\",\"install_path\":\"bin/Release/qt_desk_prepare_sources\",\"kind\":\"desk_tool\"},\"qt_desk_run\":{\"source\":\"$<TARGET_FILE:qt_desk_run>\",\"install_path\":\"bin/Release/qt_desk_run\",\"kind\":\"desk_tool\"},\"qt_evaluator\":{\"source\":\"$<TARGET_FILE:qt_evaluator>\",\"install_path\":\"bin/Release/qt_evaluator\",\"kind\":\"evaluator\"},\"qt_desk_worker\":{\"source\":\"$<TARGET_FILE:qt_desk_worker>\",\"install_path\":\"bin/Release/qt_desk_worker\",\"kind\":\"desk_worker\"}},\"require_worker\":true}\n")
    add_custom_target(release_artifacts
        COMMAND "${QT_EVALUATOR_PYTHON}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/release_artifacts.py"
            --create --inputs "${inputs}"
            --manifest "${TRADE_NGIN_RELEASE_MANIFEST}"
            --schema "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/release_artifacts_schema.json"
            --bundle-directory "${QT_EVALUATOR_BUNDLE_DIRECTORY}"
        DEPENDS ${release_dependencies}
        VERBATIM
        COMMENT "Generate the closed release-artifacts/v1 manifest from tested bytes")
endfunction()

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

# Pure bounded stdin validation; explicit operator-only file export.
add_executable(live_config_validate live_config_validate.cpp)
target_link_libraries(live_config_validate PRIVATE trade_ngin)
set_target_properties(live_config_validate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
