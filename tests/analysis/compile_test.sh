#!/bin/bash
# Compile script for analysis standalone test
# Run from tests/analysis/ directory

echo "========================================"
echo "Compiling Analysis Tools Test"
echo "========================================"
echo ""

# Check if g++ is available
if ! command -v g++ &> /dev/null; then
    echo "ERROR: C++ compiler not found!"
    echo "Please run this from MSYS2 MINGW64 terminal"
    exit 1
fi

echo "Found C++ compiler:"
g++ --version | head -n 1
echo ""

# Navigate to project root for compilation
PROJECT_ROOT="../.."

# Set include paths (from project root)
EIGEN_INCLUDE=""
if [ -d "/mingw64/include/eigen3" ]; then
    EIGEN_INCLUDE="-I/mingw64/include/eigen3"
fi

echo "Compiling from tests/analysis/..."
echo ""

# Compile the test program
g++ -std=c++17 -O2 -Wall \
    -I"${PROJECT_ROOT}/include" \
    -I"${PROJECT_ROOT}/externals/eigen" \
    ${EIGEN_INCLUDE} \
    -D_USE_MATH_DEFINES \
    analysis_standalone_test.cpp \
    ${PROJECT_ROOT}/src/analysis/statistical_distributions.cpp \
    ${PROJECT_ROOT}/src/analysis/preprocessing.cpp \
    ${PROJECT_ROOT}/src/analysis/stationarity_tests.cpp \
    -o analysis_test.exe

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "✓ Compilation successful!"
    echo "========================================"
    echo ""
    echo "Running test..."
    echo ""
    ./analysis_test.exe
    echo ""
    echo "========================================"
    echo "Test execution complete"
    echo "========================================"
else
    echo ""
    echo "========================================"
    echo "✗ Compilation failed!"
    echo "========================================"
    echo "Check the error messages above."
    exit 1
fi

echo ""
