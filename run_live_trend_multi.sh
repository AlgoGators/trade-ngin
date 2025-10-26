#!/bin/bash

# Script to run live_trend for multiple dates
# Usage:
#   ./run_live_trend_multi.sh --from 2025-01-01 --to 2025-01-03
#   ./run_live_trend_multi.sh --dates "2025-01-01 2025-01-02 2025-01-03"
#   ./run_live_trend_multi.sh --last-n-days 5
#   ./run_live_trend_multi.sh --from 2025-01-01 --to 2025-01-03 --send-email

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
LIVE_TREND_BIN="$SCRIPT_DIR/build/bin/Release/live_trend"

# Check if live_trend binary exists
if [ ! -f "$LIVE_TREND_BIN" ]; then
    echo "Error: live_trend binary not found at $LIVE_TREND_BIN"
    echo "Please build the project first with: cmake --build build --config Release"
    exit 1
fi

# Initialize variables
START_DATE=""
END_DATE=""
DATE_LIST=""
LAST_N_DAYS=""
SEND_EMAIL=""
PARALLEL=false
MAX_JOBS=4

# Function to print usage
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --from DATE         Start date (YYYY-MM-DD)"
    echo "  --to DATE           End date (YYYY-MM-DD)"
    echo "  --dates \"DATE1 DATE2 ...\"  Specific dates to run"
    echo "  --last-n-days N     Run for the last N days"
    echo "  --send-email        Pass --send-email flag to live_trend"
    echo "  --parallel          Run dates in parallel (default: sequential)"
    echo "  --max-jobs N        Maximum parallel jobs (default: 4)"
    echo ""
    echo "Examples:"
    echo "  $0 --from 2025-01-01 --to 2025-01-03"
    echo "  $0 --dates \"2025-01-01 2025-01-02 2025-01-05\""
    echo "  $0 --last-n-days 7"
    echo "  $0 --from 2025-01-01 --to 2025-01-03 --send-email --parallel"
    exit 1
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --from)
            START_DATE="$2"
            shift 2
            ;;
        --to)
            END_DATE="$2"
            shift 2
            ;;
        --dates)
            DATE_LIST="$2"
            shift 2
            ;;
        --last-n-days)
            LAST_N_DAYS="$2"
            shift 2
            ;;
        --send-email)
            SEND_EMAIL="--send-email"
            shift
            ;;
        --parallel)
            PARALLEL=true
            shift
            ;;
        --max-jobs)
            MAX_JOBS="$2"
            shift 2
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

# Function to run live_trend for a single date
run_single_date() {
    local date=$1
    local email_flag=$2
    echo "=========================================="
    echo "Running live_trend for date: $date"
    echo "=========================================="

    if $PARALLEL; then
        # In parallel mode, capture output to file
        local log_file="logs/live_trend_${date}.log"
        mkdir -p logs
        "$LIVE_TREND_BIN" "$date" $email_flag 2>&1 | tee "$log_file"
        echo "Completed: $date (log: $log_file)"
    else
        "$LIVE_TREND_BIN" "$date" $email_flag
        echo ""
        echo "Completed: $date"
        echo ""
    fi
}

# Generate date list based on input
DATES_TO_RUN=()

if [ -n "$LAST_N_DAYS" ]; then
    # Generate dates for last N days
    for ((i=LAST_N_DAYS-1; i>=0; i--)); do
        if [[ "$OSTYPE" == "darwin"* ]]; then
            # macOS
            date_str=$(date -v -${i}d "+%Y-%m-%d")
        else
            # Linux
            date_str=$(date -d "$i days ago" "+%Y-%m-%d")
        fi
        DATES_TO_RUN+=("$date_str")
    done
elif [ -n "$START_DATE" ] && [ -n "$END_DATE" ]; then
    # Generate dates from range
    current_date="$START_DATE"
    while [[ "$current_date" < "$END_DATE" ]] || [[ "$current_date" == "$END_DATE" ]]; do
        DATES_TO_RUN+=("$current_date")
        if [[ "$OSTYPE" == "darwin"* ]]; then
            # macOS
            current_date=$(date -j -f "%Y-%m-%d" -v+1d "$current_date" "+%Y-%m-%d")
        else
            # Linux
            current_date=$(date -d "$current_date + 1 day" "+%Y-%m-%d")
        fi
    done
elif [ -n "$DATE_LIST" ]; then
    # Use specific date list
    read -ra DATES_TO_RUN <<< "$DATE_LIST"
else
    echo "Error: Please specify dates using --from/--to, --dates, or --last-n-days"
    usage
fi

# Display dates to be processed
echo "Will process the following dates:"
for date in "${DATES_TO_RUN[@]}"; do
    echo "  - $date"
done
echo ""
echo "Total dates to process: ${#DATES_TO_RUN[@]}"
if $PARALLEL; then
    echo "Running in parallel mode (max jobs: $MAX_JOBS)"
else
    echo "Running in sequential mode"
fi
echo ""

# Confirm before proceeding
read -p "Continue? (y/n) " -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

echo ""
echo "Starting processing..."
echo ""

# Run live_trend for each date
if $PARALLEL; then
    # Export function and variables for parallel execution
    export -f run_single_date
    export LIVE_TREND_BIN
    export PARALLEL

    # Use GNU parallel if available, otherwise use background jobs
    if command -v parallel &> /dev/null; then
        printf '%s\n' "${DATES_TO_RUN[@]}" | parallel -j "$MAX_JOBS" run_single_date {} "$SEND_EMAIL"
    else
        # Manual parallel execution with job control
        job_count=0
        for date in "${DATES_TO_RUN[@]}"; do
            run_single_date "$date" "$SEND_EMAIL" &
            ((job_count++))

            # Wait if we've reached max jobs
            if [ $job_count -ge $MAX_JOBS ]; then
                wait -n  # Wait for any job to finish
                ((job_count--))
            fi
        done

        # Wait for all remaining jobs
        wait
    fi
else
    # Sequential execution
    for date in "${DATES_TO_RUN[@]}"; do
        run_single_date "$date" "$SEND_EMAIL"
    done
fi

echo "=========================================="
echo "All dates processed successfully!"
echo "Processed ${#DATES_TO_RUN[@]} dates:"
for date in "${DATES_TO_RUN[@]}"; do
    echo "  ✓ $date"
done
echo "==========================================="