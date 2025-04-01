#!/bin/bash
# run_backtest.sh - Script to run backtests with various configuration options

# Set default values
CONFIG_FILE=""
SAVE_CONFIG=""
CONFIG_DIR="config"
OUTPUT_DIR="apps/backtest/results"
START_DATE=""
END_DATE=""
SYMBOLS=""
CAPITAL=""
RUN_ID=""
DEBUG=false
VISUALIZE=true

# Display help
function show_help() {
    echo "Usage: $0 [options]"
    echo "Run a backtest with the specified configuration options."
    echo ""
    echo "Options:"
    echo "  -h, --help                     Display this help message"
    echo "  -c, --config <file>            Load configuration from file"
    echo "  -s, --save <file>              Save configuration to file"
    echo "  -d, --config-dir <directory>   Specify configuration directory (default: config)"
    echo "  -o, --output-dir <directory>   Specify output directory (default: apps/backtest/results)"
    echo "  --start-date <YYYY-MM-DD>      Start date for backtest"
    echo "  --end-date <YYYY-MM-DD>        End date for backtest"
    echo "  --capital <amount>             Initial capital amount"
    echo "  --symbols <sym1,sym2,...>      Comma-separated list of symbols"
    echo "  --run-id <id>                  Specify a run ID for this backtest"
    echo "  --debug                        Enable debug logging"
    echo "  --no-visualize                 Skip visualization step after backtest"
    echo ""
    echo "Examples:"
    echo "  $0 --config trend_strategy.json --capital 2000000"
    echo "  $0 --start-date 2020-01-01 --end-date 2022-12-31 --save my_config.json"
    echo "  $0 --config my_config.json --no-visualize"
}

# Parse command-line arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            show_help
            exit 0
            ;;
        -c|--config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        -s|--save)
            SAVE_CONFIG="$2"
            shift 2
            ;;
        -d|--config-dir)
            CONFIG_DIR="$2"
            shift 2
            ;;
        -o|--output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --start-date)
            START_DATE="$2"
            shift 2
            ;;
        --end-date)
            END_DATE="$2"
            shift 2
            ;;
        --capital)
            CAPITAL="$2"
            shift 2
            ;;
        --symbols)
            SYMBOLS="$2"
            shift 2
            ;;
        --run-id)
            RUN_ID="$2"
            shift 2
            ;;
        --debug)
            DEBUG=true
            shift
            ;;
        --no-visualize)
            VISUALIZE=false
            shift
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Make sure build/bin directory exists
if [ ! -d "build/bin" ]; then
    echo "Error: build/bin directory not found."
    echo "Please build the project first using CMake."
    exit 1
fi

# Check if bt_trend executable exists
if [ ! -f "build/bin/bt_trend" ]; then
    echo "Error: bt_trend executable not found."
    echo "Please build the project first using CMake."
    exit 1
fi

# Build the command
CMD="./build/bin/bt_trend"

# Add options if specified
if [ -n "$CONFIG_FILE" ]; then
    CMD="$CMD --config $CONFIG_FILE"
fi

if [ -n "$SAVE_CONFIG" ]; then
    CMD="$CMD --save $SAVE_CONFIG"
fi

if [ -n "$CONFIG_DIR" ]; then
    CMD="$CMD --config-dir $CONFIG_DIR"
fi

if [ -n "$OUTPUT_DIR" ]; then
    CMD="$CMD --output-dir $OUTPUT_DIR"
fi

if [ -n "$START_DATE" ]; then
    CMD="$CMD --start-date $START_DATE"
fi

if [ -n "$END_DATE" ]; then
    CMD="$CMD --end-date $END_DATE"
fi

if [ -n "$CAPITAL" ]; then
    CMD="$CMD --capital $CAPITAL"
fi

if [ -n "$SYMBOLS" ]; then
    CMD="$CMD --symbols $SYMBOLS"
fi

if [ -n "$RUN_ID" ]; then
    CMD="$CMD --run-id $RUN_ID"
fi

if [ "$DEBUG" = true ]; then
    CMD="$CMD --debug"
fi

# Create config directory if it doesn't exist
mkdir -p "$CONFIG_DIR"

# Create output directory if it doesn't exist
mkdir -p "$OUTPUT_DIR"

# Print the command
echo "Running backtest with command:"
echo "$CMD"
echo ""

# Execute the backtest
eval "$CMD"
BACKTEST_EXIT_CODE=$?

# Check if backtest was successful
if [ $BACKTEST_EXIT_CODE -ne 0 ]; then
    echo "Backtest failed with exit code $BACKTEST_EXIT_CODE"
    exit $BACKTEST_EXIT_CODE
fi

# Run visualization if enabled and the backtest was successful
if [ "$VISUALIZE" = true ]; then
    # Extract the run ID from the output (last line contains the run ID directory)
    RUN_DIR=$(grep -o "Results saved to: .*" -a | tail -n1 | sed 's/Results saved to: //')
    
    if [ -n "$RUN_DIR" ]; then
        echo ""
        echo "Visualizing results..."
        
        # Check if visualize_results.sh exists and is executable
        if [ -f "./visualize_results.sh" ] && [ -x "./visualize_results.sh" ]; then
            ./visualize_results.sh "$RUN_DIR"
        else
            echo "Warning: visualize_results.sh not found or not executable."
            echo "To visualize results manually, run: ./visualize_results.sh $RUN_DIR"
        fi
    else
        echo ""
        echo "Warning: Could not determine results directory for visualization."
        echo "To visualize results manually, run: ./visualize_results.sh <results_directory>"
    fi
fi

echo ""
echo "Backtest process completed."
exit 0 