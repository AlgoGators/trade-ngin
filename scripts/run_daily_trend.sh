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

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    echo -e "${RED}Error: Please run this script from the project root directory${NC}"
    exit 1
fi

# Check if config.json exists
if [ ! -f "config.json" ]; then
    echo -e "${YELLOW}Warning: config.json not found. Please ensure your database configuration is correct.${NC}"
fi

# Build the project if needed
echo -e "${BLUE}Building daily trend application...${NC}"
if [ ! -d "build" ]; then
    echo -e "${YELLOW}Build directory not found. Creating build directory...${NC}"
    mkdir -p build
fi

cd build

# Configure and build
echo -e "${BLUE}Configuring CMake...${NC}"
cmake .. -DCMAKE_BUILD_TYPE=Release

echo -e "${BLUE}Building application...${NC}"
make -j$(nproc) live_trend

# Check if build was successful
if [ ! -f "bin/live_trend" ]; then
    echo -e "${RED}Error: Build failed. Executable not found.${NC}"
    exit 1
fi

echo -e "${GREEN}Build completed successfully!${NC}"

# Go back to project root
cd ..

# Create logs directory if it doesn't exist
mkdir -p logs

# Run the daily trend application
echo -e "${BLUE}Running daily trend position generator...${NC}"
echo -e "${BLUE}Date: $(date)${NC}"
echo -e "${BLUE}========================================${NC}"

# Run the application
./build/bin/live_trend

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