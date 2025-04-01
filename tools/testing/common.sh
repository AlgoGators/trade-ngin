#!/bin/bash
# Common utility functions for Trade-Ngin testing scripts

# Color and formatting
BOLD="\033[1m"
GREEN="\033[0;32m"
YELLOW="\033[0;33m"
RED="\033[0;31m"
BLUE="\033[0;34m"
NC="\033[0m" # No Color

# Formatting functions
print_header() {
    echo -e "\n${BOLD}${GREEN}====== $1 ======${NC}\n"
}

print_subheader() {
    echo -e "\n${BOLD}${YELLOW}------ $1 ------${NC}\n"
}

print_info() {
    echo -e "${BLUE}ⓘ $1${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

# Check result of previous command
check_result() {
    if [ $? -eq 0 ]; then
        print_success "$1"
        return 0
    else
        print_error "$1"
        return 1
    fi
}

# Create timestamped directory for test results
create_results_dir() {
    local results_dir="test_results_$(date +%Y%m%d_%H%M%S)"
    mkdir -p $results_dir
    echo "Test results will be saved to $results_dir"
    echo $results_dir
}

# Detect operating system
detect_os() {
    local os_name=$(uname -s)
    print_subheader "Detected OS: $os_name"
    echo $os_name
}

# Get available CPU cores for parallel builds
get_cpu_cores() {
    local os_name=$(uname -s)
    if [ "$os_name" = "Darwin" ]; then
        echo $(sysctl -n hw.ncpu)
    else
        echo $(nproc)
    fi
}

# Check for tool availability
check_tool() {
    local tool=$1
    local message=${2:-"$tool is available"}
    
    if command -v $tool > /dev/null; then
        print_success "$message"
        return 0
    else
        print_error "$tool not found"
        return 1
    fi
}

# Clean build directory
clean_build_dir() {
    local build_dir=${1:-"build"}
    
    print_subheader "Cleaning $build_dir directory"
    if [ -d "$build_dir" ]; then
        rm -rf $build_dir
        mkdir -p $build_dir
    else
        mkdir -p $build_dir
    fi
} 