#!/bin/bash
set -e
set -x

# Set up environment
export VALGRIND_OPTS="--leak-check=full --show-leak-kinds=all --track-origins=yes"
export TSAN_OPTIONS="suppressions=/app/tsan.supp"

cd /app/build/bin

# Run memory tests
if [ -x "./valgrind_benchmark_test" ]; then
  echo "Running valgrind_benchmark_test under Valgrind..."
  valgrind $VALGRIND_OPTS ./valgrind_benchmark_test > /app/valgrind.log 2>&1
fi

# Run thread safety tests
if [ -x "./race_conditions_test" ]; then
  echo "Running race_conditions_test under ThreadSanitizer..."
  ./race_conditions_test > /app/tsan.log 2>&1
fi

# Run memory leak test if available
if [ -x "./memory_leak_test" ]; then
  echo "Running memory_leak_test under Valgrind..."
  valgrind $VALGRIND_OPTS ./memory_leak_test > /app/memleak.log 2>&1
fi

# Run minimal thread test if available
if [ -x "/app/build/bin/minimal_thread_test" ]; then
  echo "Running minimal_thread_test..."
  /app/build/bin/minimal_thread_test | tee /app/minimal_thread_test.log
fi

# Run portfolio minimal test if available
if [ -x "/app/build/bin/portfolio_minimal_test" ]; then
  echo "Running portfolio_minimal_test..."
  /app/build/bin/portfolio_minimal_test | tee /app/portfolio_minimal_test.log
fi

# Run market data minimal test if available
if [ -x "/app/build/bin/market_data_minimal_test" ]; then
  echo "Running market_data_minimal_test..."
  /app/build/bin/market_data_minimal_test | tee /app/market_data_minimal_test.log
fi

# Run signal minimal test if available
if [ -x "/app/build/bin/signal_minimal_test" ]; then
  echo "Running signal_minimal_test..."
  /app/build/bin/signal_minimal_test | tee /app/signal_minimal_test.log
fi

# Run bt_trend_minimal_test if available
if [ -x "/app/build/bin/bt_trend_minimal_test" ]; then
  echo "Running bt_trend_minimal_test..."
  /app/build/bin/bt_trend_minimal_test | tee /app/bt_trend_minimal_test.log
fi

# Analyze results
if [ -f /app/valgrind.log ]; then
  echo "Valgrind summary:" && grep "definitely lost" /app/valgrind.log || true
fi
if [ -f /app/tsan.log ]; then
  echo "ThreadSanitizer summary:" && grep "data race" /app/tsan.log || true
fi
if [ -f /app/memleak.log ]; then
  echo "Memory leak test summary:" && grep "definitely lost" /app/memleak.log || true
fi

echo "All tests complete. Check /app/*.log for full details." 