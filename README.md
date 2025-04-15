# Memory and Thread Safety Testing Environment

This repository provides a containerized environment for testing memory management and thread safety in C++ code.

## Prerequisites

- Docker
- Docker Compose

## Quick Start

1. Place your test files in the `tests` directory:
   - `valgrind_benchmark.cpp` - Memory management tests
   - `race_conditions.cpp` - Thread safety tests
   - Any additional test files

2. Create suppression files (if needed):
   - `valgrind.supp` - Valgrind suppressions
   - `tsan.supp` - Thread Sanitizer suppressions

3. Run the tests:
```bash
docker-compose up --build
```

## Test Output

The test results will be available in the `build` directory:
- `valgrind.log` - Memory leak check results
- `helgrind.log` - Thread safety analysis
- `tsan.log` - Thread Sanitizer results

## Available Tools

The container includes:
- Valgrind for memory analysis
- Helgrind for deadlock detection
- Thread Sanitizer for race condition detection
- GDB for debugging
- perf for performance analysis

## Running Individual Tests

To run specific tests or tools:

```bash
# Enter the container
docker-compose run --rm test-runner bash

# Run memory tests
valgrind --leak-check=full ./build/valgrind_benchmark

# Run thread safety tests
valgrind --tool=helgrind ./build/race_conditions

# Run with thread sanitizer
TSAN_OPTIONS="suppressions=/app/tsan.supp" ./build/race_conditions
```

## Customizing Tests

1. Add your test files to the `tests` directory
2. Update `tests/CMakeLists.txt` to include your tests
3. Rebuild and run:
```bash
docker-compose down
docker-compose up --build
``` 