#!/bin/bash
# Comprehensive test script for Trade-Ngin
# This script tests all implemented functionality

# Don't exit on error, we want to run as many tests as possible
set +e

# Get the directory of this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$TOOLS_DIR/.." && pwd)"

# Color formatting
BOLD="\033[1m"
GREEN="\033[0;32m"
YELLOW="\033[0;33m"
RED="\033[0;31m"
NC="\033[0m" # No Color

print_header() {
    echo -e "\n${BOLD}${GREEN}====== $1 ======${NC}\n"
}

print_subheader() {
    echo -e "\n${BOLD}${YELLOW}------ $1 ------${NC}\n"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

check_result() {
    if [ $? -eq 0 ]; then
        print_success "$1"
        return 0
    else
        print_error "$1"
        return 1
    fi
}

# Create directories for test results
cd "$PROJECT_ROOT"
RESULTS_DIR="test_results_$(date +%Y%m%d_%H%M%S)"
mkdir -p $RESULTS_DIR
echo "Test results will be saved to $RESULTS_DIR"

# Track overall success
TESTS_PASSED=0
TESTS_FAILED=0

# Get OS information
OS_NAME=$(uname -s)
print_subheader "Detected OS: $OS_NAME"

# ======================================================
# Test 1: Build the project
# ======================================================
print_header "Testing Build System"

print_subheader "Cleaning previous build"
if [ -d "build" ]; then
    rm -rf build
    mkdir -p build
else
    mkdir -p build
fi

print_subheader "Building with CMake"
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug > "../$RESULTS_DIR/cmake_output.txt" 2>&1
CMAKE_RESULT=$?

if [ $CMAKE_RESULT -eq 0 ]; then
    print_success "CMake configuration successful"
    
    # Determine number of CPU cores for parallel build
    if [ "$OS_NAME" = "Darwin" ]; then
        CORES=$(sysctl -n hw.ncpu)
    else
        CORES=$(nproc)
    fi
    
    # Build with make
    make -j$CORES > "../$RESULTS_DIR/make_output.txt" 2>&1
    check_result "Build with make" && ((TESTS_PASSED++)) || ((TESTS_FAILED++))
else
    print_error "CMake configuration failed"
    ((TESTS_FAILED++))
    cat "../$RESULTS_DIR/cmake_output.txt"
    # Skip dependent tests
    echo "Skipping build-dependent tests due to CMake failure"
    cd ..
    
    # Skip to tests that don't depend on the build
    goto_visualization_tests=1
fi

# ======================================================
# Test 2: Logging System (if build succeeded)
# ======================================================
if [ -z "$goto_visualization_tests" ]; then
    print_header "Testing Logging System"
    
    print_subheader "Running LogManager Test"
    # Create a simple test program for logging
    cat > ../test_logging.cpp << 'EOF'
#include "trade_ngin/core/log_manager.hpp"
#include <iostream>

using namespace trade_ngin;

int main() {
    // Initialize with global config
    LoggerConfig config;
    config.min_level = LogLevel::DEBUG;
    config.destination = LogDestination::BOTH;
    config.log_directory = "logs";
    config.filename_prefix = "test_global";
    
    // Initialize log manager
    LogManager::instance().initialize(config);
    
    // Log a message at global level
    INFO("Global logging initialized");
    
    // Configure component-specific loggers
    LogManager::instance().configure_component_logger("component1");
    LogManager::instance().configure_component_logger("component2");
    
    // Update global log level
    LogManager::instance().set_global_log_level(LogLevel::DEBUG);
    
    // Log different levels
    TRACE("This is a TRACE message (should not appear)");
    DEBUG("This is a DEBUG message");
    INFO("This is an INFO message");
    WARN("This is a WARNING message");
    ERROR("This is an ERROR message");
    
    std::cout << "Logging test completed successfully." << std::endl;
    return 0;
}
EOF
    
    # Check if the core library was built successfully
    if [ -f "lib/libtrade_ngin_core.a" ] || [ -f "lib/libtrade_ngin_core.so" ] || [ -f "lib/libtrade_ngin_core.dylib" ]; then
        # Compile the test program
        g++ -o test_logging ../test_logging.cpp -I../include -Llib -ltrade_ngin_core -std=c++17
        check_result "Compile logging test" && ((TESTS_PASSED++)) || ((TESTS_FAILED++))
        
        # Run the test program
        ./test_logging > "../$RESULTS_DIR/logging_output.txt" 2>&1
        check_result "Run logging test" && ((TESTS_PASSED++)) || ((TESTS_FAILED++))
        
        # Check if log files were created
        print_subheader "Verifying log files"
        if [ -d "logs" ]; then
            find logs -name "*.log" > "../$RESULTS_DIR/log_files.txt"
            NUM_LOGS=$(wc -l < "../$RESULTS_DIR/log_files.txt")
            if [ $NUM_LOGS -gt 0 ]; then
                print_success "Log files created: $NUM_LOGS files found"
                ((TESTS_PASSED++))
            else
                print_error "No log files found"
                ((TESTS_FAILED++))
            fi
        else
            print_error "Logs directory not created"
            ((TESTS_FAILED++))
        fi
    else
        print_error "Core library not found, skipping logging test"
        ((TESTS_FAILED++))
    fi
    
    # ======================================================
    # Test 3: Configuration Management (if build succeeded)
    # ======================================================
    print_header "Testing Configuration Management"
    
    print_subheader "Testing BacktestConfigManager"
    # Check if backtest and strategy libraries were built
    if ([ -f "lib/libtrade_ngin_backtest.a" ] || [ -f "lib/libtrade_ngin_backtest.so" ] || [ -f "lib/libtrade_ngin_backtest.dylib" ]) && \
       ([ -f "lib/libtrade_ngin_strategy.a" ] || [ -f "lib/libtrade_ngin_strategy.so" ] || [ -f "lib/libtrade_ngin_strategy.dylib" ]); then
        
        # Create a test program for configuration management
        cat > ../test_config.cpp << 'EOF'
#include "trade_ngin/backtest/backtest_config_manager.hpp"
#include "trade_ngin/core/log_manager.hpp"
#include <iostream>
#include <filesystem>

using namespace trade_ngin;
using namespace trade_ngin::backtest;

int main() {
    // Initialize logging
    LoggerConfig log_config;
    log_config.min_level = LogLevel::INFO;
    LogManager::instance().initialize(log_config);
    
    // Create test directory
    std::filesystem::create_directories("config_test");
    
    // Create default configuration
    auto result = BacktestConfigManager::create_default();
    if (result.is_error()) {
        ERROR("Failed to create default config: " + std::string(result.error()->what()));
        return 1;
    }
    
    BacktestConfigManager config_manager = result.value();
    
    // Save configuration to file
    auto save_result = config_manager.save("config_test/test_config.json");
    if (save_result.is_error()) {
        ERROR("Failed to save config: " + std::string(save_result.error()->what()));
        return 1;
    }
    
    INFO("Configuration saved successfully");
    
    // Load configuration from file
    BacktestConfigManager loaded_config("config_test");
    auto load_result = loaded_config.load("config_test/test_config.json");
    if (load_result.is_error()) {
        ERROR("Failed to load config: " + std::string(load_result.error()->what()));
        return 1;
    }
    
    INFO("Configuration loaded successfully");
    
    // Verify loaded configuration
    if (loaded_config.backtest_config().run_id != config_manager.backtest_config().run_id) {
        ERROR("Configuration mismatch");
        return 1;
    }
    
    std::cout << "Configuration test completed successfully." << std::endl;
    return 0;
}
EOF
        
        # Compile the test program
        g++ -o test_config ../test_config.cpp -I../include -Llib -ltrade_ngin_backtest -ltrade_ngin_strategy -ltrade_ngin_portfolio -ltrade_ngin_core -std=c++17 -lstdc++fs
        check_result "Compile configuration test" && ((TESTS_PASSED++)) || ((TESTS_FAILED++))
        
        # Run the test program
        ./test_config > "../$RESULTS_DIR/config_output.txt" 2>&1
        check_result "Run configuration test" && ((TESTS_PASSED++)) || ((TESTS_FAILED++))
        
        # Check if config file was created
        if [ -f "config_test/test_config.json" ]; then
            print_success "Configuration file created successfully"
            cp config_test/test_config.json "../$RESULTS_DIR/"
            ((TESTS_PASSED++))
        else
            print_error "Configuration file not created"
            ((TESTS_FAILED++))
        fi
    else
        print_error "Required libraries not found, skipping configuration test"
        ((TESTS_FAILED++))
    fi
    
    # Return to project root
    cd ..
fi

# ======================================================
# Test 4: Visualization
# ======================================================
print_header "Testing Visualization"

print_subheader "Generating sample data"

if [ -f "$TOOLS_DIR/visualization/generate_sample_data.py" ]; then
    # Run the sample data generator
    python "$TOOLS_DIR/visualization/generate_sample_data.py" --output-dir=test_backtest_data
    check_result "Generate sample data" && ((TESTS_PASSED++)) || ((TESTS_FAILED++))
else
    print_error "Sample data generator not found at $TOOLS_DIR/visualization/generate_sample_data.py"
    ((TESTS_FAILED++))
fi

print_subheader "Running visualization script"
if [ -f "$TOOLS_DIR/visualization/visualize_results.sh" ]; then
    # Run visualization script
    bash "$TOOLS_DIR/visualization/visualize_results.sh" test_backtest_data > "$RESULTS_DIR/visualization_output.txt" 2>&1
    check_result "Run visualization script" && ((TESTS_PASSED++)) || ((TESTS_FAILED++))
else
    print_error "Visualization script not found at $TOOLS_DIR/visualization/visualize_results.sh"
    ((TESTS_FAILED++))
fi

# Check if charts were created
print_subheader "Verifying visualization results"
if [ -d "test_backtest_data/charts" ]; then
    CHART_COUNT=$(find test_backtest_data/charts -name "*.png" | wc -l)
    if [ $CHART_COUNT -gt 0 ]; then
        print_success "Charts created successfully: $CHART_COUNT charts"
        # Copy some charts to results dir
        cp test_backtest_data/charts/performance_dashboard.png $RESULTS_DIR/
        ((TESTS_PASSED++))
    else
        print_error "No charts were created"
        ((TESTS_FAILED++))
    fi
else
    print_error "Charts directory not created"
    ((TESTS_FAILED++))
fi

# ======================================================
# Test 5: Docker Environment (if available)
# ======================================================
print_header "Testing Docker Environment"

if command -v docker > /dev/null && command -v docker-compose > /dev/null; then
    print_subheader "Checking Docker configuration"
    
    # Check if Dockerfile exists
    if [ -f "$TOOLS_DIR/docker/Dockerfile" ]; then
        print_success "Dockerfile exists"
        ((TESTS_PASSED++))
        
        # Check for specific Arrow repository fix
        if grep -q "apache.jfrog.io/artifactory/arrow" "$TOOLS_DIR/docker/Dockerfile"; then
            print_success "Arrow repository URL is up-to-date"
            ((TESTS_PASSED++))
        else
            print_error "Arrow repository URL might need updating"
            ((TESTS_FAILED++))
        fi
    else
        print_error "Dockerfile not found"
        ((TESTS_FAILED++))
    fi
    
    print_subheader "Checking Docker Compose configuration"
    if [ -f "$TOOLS_DIR/docker/docker-compose.yml" ]; then
        print_success "docker-compose.yml exists"
        ((TESTS_PASSED++))
    else
        print_error "docker-compose.yml not found"
        ((TESTS_FAILED++))
    fi
    
    # Skip Docker build by default as it takes too long
    print_subheader "Docker build test"
    print_success "Docker build test skipped (would take too long)"
    print_subheader "To test Docker build manually, run:"
    echo "cd $TOOLS_DIR/docker && docker-compose build --no-cache"
else
    echo "Docker not available, skipping Docker tests"
fi

# ======================================================
# Test 6: Check Documentation
# ======================================================
print_header "Testing Documentation"

print_subheader "Checking README files"
DOC_FILES=("README.md" "tools/docs/README_DEPENDENCIES.md" "tools/docs/README_TESTING.md" "tools/docs/README_TOOLS.md" "tools/docs/IMPLEMENTATION_STATUS.md")
for doc in "${DOC_FILES[@]}"; do
    if [ -f "$doc" ]; then
        print_success "$doc exists"
        ((TESTS_PASSED++))
    else
        print_error "$doc not found"
        ((TESTS_FAILED++))
    fi
done

# ======================================================
# Summary
# ======================================================
print_header "Test Summary"

echo -e "${BOLD}Tests passed: ${GREEN}$TESTS_PASSED${NC}"
echo -e "${BOLD}Tests failed: ${RED}$TESTS_FAILED${NC}"

echo -e "\nTest results saved to: ${BOLD}$RESULTS_DIR${NC}"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "\n${GREEN}All tests passed successfully!${NC}"
    exit 0
else
    echo -e "\n${RED}Some tests failed. Please check the results directory for details.${NC}"
    exit 1
fi 