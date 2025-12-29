# Analysis Module Tests

This directory contains tests for the analysis module.

## Quick Start

From your **MSYS2 MINGW64 terminal**:

```bash
# Navigate to this directory
cd /c/Users/hackathon/Documents/GitHub/trade-ngin/tests/analysis

# Make the script executable
chmod +x compile_test.sh

# Run the test
./compile_test.sh
```

## What Gets Tested

- ✅ Statistical distributions (Normal, t, chi-square)
- ✅ Preprocessing (z-score, min-max, robust scaling)
- ✅ Stationarity tests (ADF, KPSS)
- ✅ PCA (dimensionality reduction)

## Expected Output

```
========================================
Compiling Analysis Tools Test
========================================

Found C++ compiler:
g++ (Rev8, Built by MSYS2 project) 15.2.0

Compiling from tests/analysis/...

========================================
✓ Compilation successful!
========================================

Running test...

==================================
  Analysis Tools Standalone Test
==================================

=== Testing Statistical Distributions ===
Normal CDF at z=1.96: 0.975 (expected ~0.975)
Normal quantile at p=0.975: 1.96 (expected ~1.96)
Chi-square CDF(5, df=3): 0.828...
✓ Statistical distributions working

=== Testing Preprocessing ===
Generated 100 price points
Price range: 100 to ~120
✓ Z-score normalization successful
  Normalized mean (should be ~0): 0.000...
✓ Min-max scaling successful
  Range: [0, 1]
✓ Returns calculation successful
  Mean return: ~0.1%

=== Testing Stationarity Tests ===

Testing Random Walk (should be NON-stationary):
  ADF statistic: -1.5 (example)
  Critical value (5%): -3.00
  Is stationary: NO

Testing White Noise (should be stationary):
  ADF statistic: -8.5 (example)
  Critical value (5%): -3.00
  Is stationary: YES

KPSS Test on White Noise:
  KPSS statistic: 0.15
  Critical value (5%): 0.463
  Is stationary: YES

=== Testing PCA ===
✓ PCA fit successful
  Explained variance ratios:
    PC1: 68.5%
    PC2: 21.2%
  Original dimensions: 4
  Reduced dimensions: 2

==================================
  ✓ All Tests Completed!
==================================
```

## Manual Compilation

If you prefer to compile manually:

```bash
g++ -std=c++17 -O2 \
    -I"../../include" \
    -I"../../externals/eigen" \
    -I"/mingw64/include/eigen3" \
    analysis_standalone_test.cpp \
    ../../src/analysis/statistical_distributions.cpp \
    ../../src/analysis/preprocessing.cpp \
    ../../src/analysis/stationarity_tests.cpp \
    -o analysis_test.exe

./analysis_test.exe
```

## Include Paths

When the test is in `tests/analysis/`, the include paths are:

- Headers: `#include "trade_ngin/analysis/preprocessing.hpp"`
- Compiler flag: `-I"../../include"` (goes up 2 levels to project root)
- Eigen: `-I"../../externals/eigen"` (goes up 2 levels)

## Troubleshooting

### "trade_ngin/analysis/... No such file or directory"

Make sure you're compiling with the correct include paths:
- You need `-I"../../include"` to find `trade_ngin/` headers
- You need `-I"../../externals/eigen"` for Eigen

### "g++: command not found"

You're not in MSYS2 MINGW64 terminal. Open it from Start Menu:
- Search: "MSYS2 MINGW64"
- Not "MSYS2 MSYS" or regular CMD

### Permission denied

```bash
chmod +x compile_test.sh
```

## Integration with CMake

For proper integration with the build system, this test should be added to `tests/CMakeLists.txt`:

```cmake
add_executable(analysis_standalone_test
    analysis/analysis_standalone_test.cpp
)

target_link_libraries(analysis_standalone_test
    PRIVATE trade_ngin
)

add_test(NAME AnalysisStandaloneTest
    COMMAND analysis_standalone_test
)
```

This way you can run it with `ctest` once the full build is set up.
