option(ENABLE_COVERAGE "Enable code coverage" OFF)

if(ENABLE_COVERAGE AND NOT CMAKE_BUILD_TYPE MATCHES "Debug")
    message(WARNING "Code coverage results with an optimized (non-Debug) build may be misleading")
endif()

if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Add coverage flags as separate options
        set(COVERAGE_COMPILE_FLAGS -g -O0 --coverage -fprofile-arcs -ftest-coverage)
        set(COVERAGE_LINK_FLAGS --coverage)
        
        message(STATUS "Building with code coverage enabled")
        
        # Function to add coverage flags to a target
        function(add_coverage_target target_name)
            target_compile_options(${target_name} PRIVATE ${COVERAGE_COMPILE_FLAGS})
            target_link_options(${target_name} PRIVATE ${COVERAGE_LINK_FLAGS})
        endfunction()
        
        # Find required tools
        find_program(LCOV_PATH lcov)
        find_program(GENHTML_PATH genhtml)
        
        if(NOT LCOV_PATH)
            message(WARNING "lcov not found! Coverage report generation will not be available.")
        endif()
        
        if(NOT GENHTML_PATH)
            message(WARNING "genhtml not found! HTML coverage report generation will not be available.")
        endif()
        
        if(LCOV_PATH AND GENHTML_PATH)
            # Add custom target for generating coverage report
            add_custom_target(coverage
                # Cleanup lcov
                COMMAND ${LCOV_PATH} --directory . --zerocounters
                
                # Run the full suite with each Google Test case isolated. The
                # monolithic binary has known order-dependent test failures.
                # Keep test temp files separate from other worktrees' runs.
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/coverage-tmp"
                COMMAND ${CMAKE_COMMAND} -E env "TMPDIR=${CMAKE_BINARY_DIR}/coverage-tmp" ${CMAKE_CTEST_COMMAND} --output-on-failure -j 1
                
                # Capture coverage info (ignore errors for Apple Clang compatibility)
                COMMAND ${LCOV_PATH} --directory CMakeFiles/trade_ngin.dir --capture --output-file coverage.info --rc lcov_branch_coverage=1 --ignore-errors inconsistent,unsupported,range,format,count,negative,mismatch
                
                # Keep only this checkout's src/; external dependencies may also have a src/ directory.
                COMMAND ${LCOV_PATH} --extract coverage.info "${CMAKE_SOURCE_DIR}/src/*" --output-file coverage.info.cleaned --rc lcov_branch_coverage=1 --ignore-errors inconsistent,unsupported,format,count,unused
                
                # Generate HTML report with branch coverage
                COMMAND ${GENHTML_PATH} -o coverage coverage.info.cleaned --branch-coverage --rc lcov_branch_coverage=1 --rc genhtml_branch_coverage=1 --function-coverage --legend --ignore-errors inconsistent,unsupported,format,count,negative,category
                
                # Print summary with branch coverage
                COMMAND ${LCOV_PATH} --summary coverage.info.cleaned --rc lcov_branch_coverage=1 --ignore-errors inconsistent,unsupported,format,count
                
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Generating code coverage report..."
                VERBATIM
            )
            
            message(STATUS "Coverage target 'coverage' added. Run with: make coverage")
        endif()
    else()
        message(WARNING "Code coverage is only supported with GCC or Clang compilers")
    endif()
endif()
