#!/bin/bash
# Build system testing module for Trade-Ngin

# Import common utilities
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

# Run build tests
# Parameters:
# $1: results directory
# $2: track passed tests variable name (for incrementing)
# $3: track failed tests variable name (for incrementing)
run_build_tests() {
    local results_dir=$1
    local passed_var=$2
    local failed_var=$3
    
    # Create local variables to track test results within this module
    local tests_passed=0
    local tests_failed=0
    
    print_header "Testing Build System"
    
    # Clean build directory
    clean_build_dir "build"
    
    # Test CMake configuration
    print_subheader "Running CMake"
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Debug > "../$results_dir/cmake_output.txt" 2>&1
    local cmake_result=$?
    
    if [ $cmake_result -eq 0 ]; then
        print_success "CMake configuration successful"
        
        # Determine CPU cores for parallel build
        local cores=$(get_cpu_cores)
        
        # Build with make
        print_subheader "Building with Make"
        make -j$cores > "../$results_dir/make_output.txt" 2>&1
        if check_result "Build with make"; then
            tests_passed=$((tests_passed + 1))
        else
            tests_failed=$((tests_failed + 1))
            print_info "Check $results_dir/make_output.txt for details"
        fi
    else
        print_error "CMake configuration failed"
        tests_failed=$((tests_failed + 1))
        cat "../$results_dir/cmake_output.txt"
        
        # Return to root directory
        cd ..
        
        # Return special error code to indicate CMake failure
        # This will allow the main script to skip dependent tests
        return 100
    fi
    
    # Check if libraries were built
    print_subheader "Checking built libraries"
    if [ -d "lib" ]; then
        local lib_count=$(find lib -name "libtrade_ngin_*.a" -o -name "libtrade_ngin_*.so" -o -name "libtrade_ngin_*.dylib" | wc -l)
        if [ $lib_count -gt 0 ]; then
            print_success "Libraries built successfully: $lib_count found"
            tests_passed=$((tests_passed + 1))
            # List libraries
            find lib -name "libtrade_ngin_*.a" -o -name "libtrade_ngin_*.so" -o -name "libtrade_ngin_*.dylib" > "../$results_dir/libraries.txt"
        else
            print_error "No libraries were built"
            tests_failed=$((tests_failed + 1))
        fi
    else
        print_error "Library directory not found"
        tests_failed=$((tests_failed + 1))
    fi
    
    # Return to root directory
    cd ..
    
    # Print module summary
    print_subheader "Build Test Summary"
    print_info "Tests passed: $tests_passed"
    print_info "Tests failed: $tests_failed"
    
    # Update the passed and failed test counters in the parent script
    eval "$passed_var=$((${!passed_var} + tests_passed))"
    eval "$failed_var=$((${!failed_var} + tests_failed))"
    
    # Return success (0) if all tests passed, otherwise return the number of failed tests
    return $tests_failed
} 