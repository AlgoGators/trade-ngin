#!/bin/bash
set -e

# Move to the trade-ngin directory where CMakeLists.txt is located
cd /app/trade-ngin

# Clean up any existing build artifacts
rm -rf build

# Create build directory
mkdir -p build
cd build

# Configure and build
cmake .. -DENABLE_POSTGRESQL=OFF -DENABLE_TESTS=ON
make -j$(nproc)

# Run memory tests
echo "Running memory tests..."
valgrind --tool=memcheck \
    --leak-check=full \
    --show-leak-kinds=all \
    --track-origins=yes \
    --suppressions=/app/valgrind.supp \
    ./tests/valgrind_benchmark 2>&1 | tee /app/valgrind.log

# Run thread safety tests
echo "Running thread safety tests..."
valgrind --tool=helgrind \
    ./tests/race_conditions 2>&1 | tee /app/helgrind.log

# Run with ThreadSanitizer
TSAN_OPTIONS="suppressions=/app/tsan.supp" \
    ./tests/race_conditions 2>&1 | tee /app/tsan.log

# Print test results
echo "Test results:"
echo "Memory test log: /app/valgrind.log"
echo "Thread safety test logs: /app/helgrind.log and /app/tsan.log" 