#!/bin/bash

# Colors for better readability
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}Starting memory management and thread safety testing${NC}"

# Create logs directory if it doesn't exist
mkdir -p logs

# Build and run the Docker container
echo -e "${YELLOW}Building Docker container for memory tests...${NC}"
docker-compose -f docker-compose.memory-tests.yml build

echo -e "${YELLOW}Running memory tests in Docker container...${NC}"
docker-compose -f docker-compose.memory-tests.yml up

echo -e "${GREEN}Tests completed!${NC}"
echo -e "${YELLOW}Check the logs directory for test results:${NC}"
echo -e "- Memory leak tests: logs/memory_leak_test.log"
echo -e "- Race condition tests: logs/race_conditions_test.log"
echo -e "- Valgrind benchmark tests: logs/valgrind_benchmark_test.log"
echo -e "- Heap profile: logs/massif_report.txt" 