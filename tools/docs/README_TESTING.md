# Trade-Ngin Testing Guide

This document provides instructions for testing Trade-Ngin's functionality.

## Overview

The Trade-Ngin project includes several testing tools:

1. **Comprehensive Test Script**: A bash script that tests all implemented functionality
2. **Sample Data Generator**: A Python script that generates realistic backtest data
3. **Visualization Script**: A shell script that creates visualizations from backtest data

## Comprehensive Testing

The `test_all_implementations.sh` script performs end-to-end testing of the Trade-Ngin codebase:

```bash
./test_all_implementations.sh
```

This script tests:
- Build system
- Logging system
- Configuration management
- Visualization capabilities
- Docker environment
- Documentation

### Test Results

Results are saved to a timestamped directory (e.g., `test_results_20250331_005058`). This directory contains:
- Build system logs
- Logging test output
- Configuration test output
- Visualization test output
- Sample visualizations

## Manual Testing

### Generating Sample Data

You can generate sample backtest data using:

```bash
./generate_sample_data.py --output-dir=<your_directory>
```

Options:
- `--output-dir`: Directory where sample data will be generated (default: `test_backtest_data`)

The script creates:
- `equity_curve.csv`: Equity curve with drawdowns
- `trades.csv`: Individual trade data
- `symbol_pnl.csv`: P&L breakdown by symbol
- `monthly_returns.csv`: Monthly return data
- `turnover.csv`: Portfolio turnover data
- `results.csv`: Summary performance metrics

### Visualizing Backtest Results

To visualize backtest results:

```bash
./visualize_results.sh <backtest_directory>
```

Where `<backtest_directory>` is a directory containing backtest data (CSV files). The script creates visualizations in a `charts` subdirectory, including:

- `equity_curve.png`: Equity curve with drawdowns
- `monthly_returns.png`: Monthly returns heatmap
- `trade_pnl_distribution.png`: Distribution of trade profits/losses
- `pnl_by_symbol.png`: Profit/loss by symbol
- `monthly_turnover.png`: Monthly portfolio turnover
- `performance_dashboard.png`: Summary dashboard with key metrics

## Troubleshooting

### Arrow Compatibility Issues

If you encounter Arrow compatibility issues during testing:

```
CMake Error: Could not find a configuration file for package "Arrow" that is compatible with requested version "6.0.0".
```

Possible solutions:
1. **Use Docker**: The Docker environment has the correct Arrow version
   ```bash
   docker-compose run --rm trade-ngin
   ```

2. **Install Arrow 6.0.0**: Install the specific version required
   ```bash
   # For Ubuntu:
   apt-get install -y libarrow-dev=6.0.0
   
   # For macOS:
   brew install apache-arrow@6.0.0
   ```

3. **Update CMakeLists.txt**: Modify the Arrow version requirement to match your system
   ```cmake
   find_package(Arrow 11.0.0 REQUIRED)  # Adjust version as needed
   ```

### CMake Cache Issues

If switching between Docker and local builds:

```bash
# Clean build directory
rm -rf build
mkdir build
cd build
cmake ..
```

### Visualization Issues

If visualization fails:
1. Ensure required Python packages are installed:
   ```bash
   pip install matplotlib seaborn pandas numpy
   ```

2. Check that the data directory contains the expected CSV files:
   ```bash
   ls -la <backtest_directory>
   ```

## Adding New Tests

To add new tests to the comprehensive test script:
1. Open `test_all_implementations.sh`
2. Add a new test section following the existing pattern:
   ```bash
   # ======================================================
   # Test X: Your New Test
   # ======================================================
   print_header "Testing Your Feature"
   
   print_subheader "Running your test"
   # Your test commands here
   check_result "Your test description" && ((TESTS_PASSED++)) || ((TESTS_FAILED++))
   ``` 