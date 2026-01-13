#!/bin/bash
# Equity Mean Reversion Daily Paper Trading Script
# Runs the live equity mean reversion strategy for the current date
# Usage: Called automatically by cron or manually for specific dates

DATE=$(date +%Y-%m-%d)
echo "==================================================="
echo "Running Equity Mean Reversion for: $DATE"
echo "==================================================="

cd /app

# Run the equity mean reversion executable
/app/build/bin/Release/live_equity_mean_reversion "$DATE" --send-email

EXIT_CODE=$?

if [ $EXIT_CODE -eq 0 ]; then
    echo "✓ Equity trading completed successfully for $DATE"
else
    echo "✗ Equity trading failed for $DATE (exit code: $EXIT_CODE)"
fi

exit $EXIT_CODE
