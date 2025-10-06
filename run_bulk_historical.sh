#!/bin/bash

# Script to run live_trend for bulk historical dates
# Usage: ./run_bulk_historical.sh [start_date] [end_date]
# Example: ./run_bulk_historical.sh 2025-01-11 2025-10-04

if [ $# -ne 2 ]; then
    echo "Usage: $0 <start_date> <end_date>"
    echo "Example: $0 2025-01-11 2025-10-04"
    exit 1
fi

start_date="$1"
end_date="$2"

echo "========================================="
echo "Running live_trend for bulk historical dates"
echo "From: $start_date to $end_date"
echo "========================================="

# Convert dates to seconds since epoch for comparison
start_epoch=$(date -j -f "%Y-%m-%d" "$start_date" "+%s" 2>/dev/null)
end_epoch=$(date -j -f "%Y-%m-%d" "$end_date" "+%s" 2>/dev/null)

if [ -z "$start_epoch" ] || [ -z "$end_epoch" ]; then
    echo "Error: Invalid date format. Please use YYYY-MM-DD"
    exit 1
fi

if [ "$start_epoch" -gt "$end_epoch" ]; then
    echo "Error: Start date must be before or equal to end date"
    exit 1
fi

# Process each date
current_epoch=$start_epoch
processed_count=0
failed_count=0

while [ "$current_epoch" -le "$end_epoch" ]; do
    # Convert epoch back to date string
    current_date=$(date -j -r "$current_epoch" "+%Y-%m-%d")

    echo ""
    echo "Processing: $current_date"
    echo "----------------------------------------"

    # Run live_trend with the specific date
    ./build/bin/Release/live_trend "$current_date"

    # Check if the command was successful
    if [ $? -eq 0 ]; then
        echo "✓ Successfully processed $current_date"
        ((processed_count++))
    else
        echo "✗ Failed to process $current_date"
        ((failed_count++))
        # Continue processing even if one date fails
    fi

    # Move to next day (86400 seconds = 1 day)
    current_epoch=$((current_epoch + 86400))

    # Small delay between runs
    sleep 1
done

echo ""
echo "========================================="
echo "Bulk processing completed!"
echo "Processed: $processed_count dates successfully"
echo "Failed: $failed_count dates"
echo "========================================="

if [ $failed_count -gt 0 ]; then
    echo "Warning: Some dates failed to process. Review logs for details."
    exit 1
fi