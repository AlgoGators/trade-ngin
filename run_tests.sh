#!/bin/bash
set -e
mkdir -p build && cd build
cmake .. -DENABLE_POSTGRESQL=OFF -DENABLE_TESTS=ON
make -j$(nproc)
echo "Running memory tests..."
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all --track-origins=yes --suppressions=/app/valgrind.supp ./tests/valgrind_benchmark 2>&1 | tee /app/valgrind.log
echo "Running thread safety tests..."
valgrind --tool=helgrind ./tests/race_conditions 2>&1 | tee /app/helgrind.log
./tests/race_conditions 2>&1 | tee /app/tsan.log
echo "Test results:"
echo "Memory test log: /app/valgrind.log"
echo "Thread safety test logs: /app/helgrind.log and /app/tsan.log" 