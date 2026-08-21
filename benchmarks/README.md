# trade-ngin Benchmark Harness

## Overview

This directory contains a self-contained performance benchmark suite for trade-ngin's critical paths.

**Purpose**: Establish a reproducible baseline before refactoring to prove "no performance regression."

**Key design choice**: Median + p95, not mean. Mean is dragged up by scheduler noise, making unchanged code look slower. Median is stable; p95 catches tail regressions.

## Running Benchmarks

### Build the benchmark target

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DNLopt_DIR=/usr/lib/x86_64-linux-gnu/cmake/nlopt
cmake --build build --target trade_ngin_bench
```

### Run benchmarks (human-readable output)

```bash
./build/bin/Release/trade_ngin_bench
```

### Run benchmarks and save JSON baseline

```bash
./build/bin/Release/trade_ngin_bench --json benchmarks/baseline.json
```

### Run benchmarks and save as current result

```bash
./build/bin/Release/trade_ngin_bench --json /tmp/current.json
```

### Compare current vs. baseline

```bash
python3 benchmarks/compare.py benchmarks/baseline.json /tmp/current.json
```

### Compare with custom threshold (default 10%)

```bash
python3 benchmarks/compare.py benchmarks/baseline.json /tmp/current.json --threshold 5
```

## Benchmarks Included

1. **Optimizer_10_symbols** - Position optimizer with 10 contracts, 10 iterations
2. **Optimizer_50_symbols** - Position optimizer with 50 contracts (realistic scale)
3. **RiskManager_10_symbols** - Risk calculations (VaR, leverage, correlation) with 10 positions
4. **RiskManager_50_symbols** - Risk calculations with 50 positions
5. **BarConversion_10_symbols** - Market data row conversion (pqxx result → Bar)
6. **Statistics_Normalizer_10** - Z-score normalization over 100 samples × 10 features
7. **Statistics_ADF_test_10** - Stationarity test (Augmented Dickey-Fuller)

Each benchmark runs:
- **5 warmup iterations** (let caches warm, branch predictors stabilize)
- **100 timed iterations** (deterministic synthetic input, fixed seed)
- Reports **median** and **p95** from sorted times

## Interpreting Results

### Baseline Machine

All times are in **nanoseconds** but are **only meaningful on the same hardware**. Absolute values are not comparable across machines with different CPUs, RAM, thermal profiles, or OS load.

**The signal is the ratio, not the absolute nanoseconds.**

Example baseline (your machine):
```
Optimizer_10_symbols         |      12345 ns (median) |      15000 ns (p95)
RiskManager_50_symbols       |      45678 ns (median) |      52000 ns (p95)
```

After refactoring, re-generate:
```bash
./build/bin/Release/trade_ngin_bench --json /tmp/current.json
python3 benchmarks/compare.py benchmarks/baseline.json /tmp/current.json
```

If the script exits 0, no regression. If exit 1, it lists which benchmarks slowed down and by how much.

### Regenerating Baseline

When the baseline was measured on machine A, you cannot compare against it on machine B. Regenerate:

```bash
# Run on your machine now
./build/bin/Release/trade_ngin_bench --json benchmarks/baseline.json
git add benchmarks/baseline.json
git commit -m "Regenerate benchmark baseline for current hardware"
```

Update the `baseline.json` header comment (see below) to note the machine.

## Baseline Format

`benchmarks/baseline.json` is a JSON array of benchmark results:

```json
[
  {
    "name": "Optimizer_10_symbols",
    "iterations": 100,
    "median_ns": 12345,
    "p95_ns": 15000
  },
  ...
]
```

**Important**: Regenerating baseline invalidates comparisons with old runs unless you document the machine. Add a header comment in git commits:

```
Regenerate benchmark baseline for current hardware
- Measured on: [CPU model, OS, RAM config]
- Date: [date]
- Note: Baseline is now only comparable to runs on this machine
```

## Implementation Details

### Why do_not_optimize?

The benchmark workload is protected by an asm barrier (`do_not_optimize()`). Without it, the compiler can:
- Constant-fold computations
- Eliminate dead stores
- Optimize away entire workloads

Result: you measure nothing, get a fake fast number, and miss real regressions.

### Why Synthetic Data?

All benchmarks generate input in-process with a fixed seed:
- No database, network, or file I/O in timed sections
- Deterministic (same input every run)
- Reproducible across machines

### Warmup Phase

First 5 iterations warm up:
- CPU caches
- Branch predictors
- Instruction prefetchers

This stabilizes subsequent timing.

## Troubleshooting

### `trade_ngin_bench` binary not found

```bash
cmake --build build --target trade_ngin_bench -v
# Check for errors in the build log
```

### Comparison script fails

```bash
# Check file format
python3 -m json.tool benchmarks/baseline.json | head -20

# Run compare.py directly to see the error
python3 benchmarks/compare.py benchmarks/baseline.json /tmp/current.json
```

### Times vary wildly between runs

This is normal. Reasons:
- System load (background processes)
- Cache state
- CPU frequency scaling
- THP (transparent huge pages) state

Use median/p95, not individual runs. If you see **systematic** slowdowns across all runs, there's a real regression.

## References

- **Median vs. Mean**: Percentiles are robust to outliers; mean is not. See Brendan Gregg's latency percentile analysis.
- **do_not_optimize**: Inspired by Google Benchmark's approach; prevents compiler optimizations from invalidating the benchmark.
- **Fixed Seed**: Ensures bit-exact reproducibility; input variance doesn't confound code-path variance.
