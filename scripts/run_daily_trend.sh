#!/bin/bash

# Daily Trend Following Position Generator
# This script runs the daily trend following strategy to generate positions and metrics

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Daily Trend Following Position Generator${NC}"
echo -e "${BLUE}========================================${NC}"

# Run the daily trend application
echo -e "${BLUE}Running daily trend position generator...${NC}"
echo -e "${BLUE}Date: $(date)${NC}"
echo -e "${BLUE}========================================${NC}"

# Run the application
cd /app
./build/bin/Release/live_trend

# Check exit code
if [ $? -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Daily trend processing completed successfully!${NC}"
    echo -e "${GREEN}Check the logs directory for detailed logs.${NC}"
    echo -e "${GREEN}Check the current directory for position CSV files.${NC}"
    echo -e "${GREEN}========================================${NC}"
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}Daily trend processing failed!${NC}"
    echo -e "${RED}Check the logs for error details.${NC}"
    echo -e "${RED}========================================${NC}"
    exit 1
fi
