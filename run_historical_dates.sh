#!/bin/bash

# Script to run live_trend for historical dates
# Usage: ./run_historical_dates.sh

echo "========================================="
echo "Running live_trend for historical dates"
echo "========================================="

# Array of dates to process (Jan 1-10, 2025)
dates=(
    "2025-01-01"
    "2025-01-02"
    "2025-01-03"
    "2025-01-04"
    "2025-01-05"
    "2025-01-06"
    "2025-01-07"
    "2025-01-08"
    "2025-01-09"
    "2025-01-10"
)

# Process each date
for date in "${dates[@]}"
do
    echo ""
    echo "========================================="
    echo "Processing date: $date"
    echo "========================================="

    # Run live_trend with the specific date (no email for historical backfill)
    ./build/bin/Release/live_trend "$date"
    # To send emails, add --send-email flag:
    # ./build/bin/Release/live_trend "$date" --send-email

    # Check if the command was successful
    if [ $? -eq 0 ]; then
        echo "✓ Successfully processed $date"
    else
        echo "✗ Failed to process $date"
        echo "Stopping script due to error"
        exit 1
    fi

    # Small delay between runs to avoid overwhelming the system
    sleep 2
done

echo ""
echo "========================================="
echo "All dates processed successfully!"
echo "========================================="
echo ""
echo "Next steps:"
echo "1. Review the database to verify positions and metrics"
echo "2. Check the daily_positions_*.csv files generated"
echo "3. If everything looks good, run bulk processing for remaining dates"