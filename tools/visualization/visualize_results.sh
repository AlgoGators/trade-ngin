#!/bin/bash
# Backtest Visualization Helper Script
#
# This script automatically finds the most recent backtest results directory
# and runs the Python visualization tool to generate charts.
#
# Usage:
#   ./visualize_results.sh                  # Uses the most recent backtest results
#   ./visualize_results.sh <results_dir>    # Uses a specific results directory
#
# Requirements:
#   - Python 3.6+ with pandas, matplotlib, seaborn, numpy
#   - Backtest results in CSV format

set -e

# Get the directory of this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Check if a specific directory was provided
if [ $# -eq 1 ]; then
    results_dir="$1"
else
    # Find the most recent backtest results directory
    results_dir=$(ls -td ./apps/backtest/results/BT_* 2>/dev/null | head -1)
    
    # If no directories found, try relative path
    if [ -z "$results_dir" ]; then
        results_dir=$(ls -td apps/backtest/results/BT_* 2>/dev/null | head -1)
    fi
fi

if [ -z "$results_dir" ]; then
    echo "Error: No backtest results found!"
    echo "Run a backtest first or specify a results directory."
    exit 1
fi

echo "Visualizing results from: $results_dir"

# Check for Python dependencies and install if needed
if ! python3 -c "import pandas, matplotlib, seaborn, numpy" &>/dev/null; then
    echo "Installing required Python packages..."
    pip3 install pandas matplotlib seaborn numpy
fi

# Run visualization script
python3 "$SCRIPT_DIR/visualize_backtest.py" --results-dir "$results_dir"

# Open the performance dashboard
chart_path="${results_dir}/charts/performance_dashboard.png"
if [ -f "$chart_path" ]; then
    echo "Opening performance dashboard..."
    if [ "$(uname)" == "Darwin" ]; then
        open "$chart_path"  # macOS
    elif [ "$(expr substr $(uname -s) 1 5)" == "Linux" ]; then
        xdg-open "$chart_path" 2>/dev/null || echo "Dashboard saved to: $chart_path"  # Linux
    elif [ "$(expr substr $(uname -s) 1 10)" == "MINGW32_NT" ]; then
        start "$chart_path"  # Windows
    else
        echo "Dashboard saved to: $chart_path"
    fi
fi

echo "Visualization complete!"
echo "All charts saved to: ${results_dir}/charts/" 