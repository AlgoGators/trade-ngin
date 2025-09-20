#!/bin/bash

# Docker entrypoint script for Trade Ngin
# Supports both cron mode and direct execution mode

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Trade Ngin Docker Container${NC}"
echo -e "${BLUE}========================================${NC}"

# Function to start cron daemon
start_cron() {
    echo -e "${BLUE}Starting cron daemon...${NC}"

    # Create log directory
    mkdir -p /var/log/trade_ngin

    # Start cron daemon in foreground
    echo -e "${GREEN}Cron daemon started. Container will run continuously.${NC}"
    echo -e "${YELLOW}Daily trend strategy will run at 9:30 AM Eastern Time${NC}"
    echo -e "${YELLOW}Logs will be written to /var/log/trade_ngin/cron.log${NC}"

    # Start cron in foreground
    exec cron -f
}

# Function to run live trend directly
run_live_trend() {
    echo -e "${BLUE}Running live trend strategy directly...${NC}"
    exec /app/run_daily_trend.sh
}

# Function to run backtest
run_backtest() {
    echo -e "${BLUE}Running backtest...${NC}"
    exec /app/startup.sh "$@"
}

# Function to show help
show_help() {
    echo -e "${YELLOW}Usage:${NC}"
    echo -e "  docker run <image> cron          # Start cron daemon (default)"
    echo -e "  docker run <image> live          # Run live trend strategy once"
    echo -e "  docker run <image> backtest      # Run backtest"
    echo -e "  docker run <image> help          # Show this help"
    echo -e ""
    echo -e "${YELLOW}Environment Variables:${NC}"
    echo -e "  MODE=cron|live|backtest         # Set execution mode"
    echo -e ""
    echo -e "${YELLOW}Examples:${NC}"
    echo -e "  docker run <image>"
    echo -e "  docker run <image> live"
    echo -e "  docker run <image> backtest"
    echo -e "  MODE=live docker run <image>"
}

# Main logic
case "${1:-cron}" in
    "cron")
        start_cron
        ;;
    "live")
        run_live_trend
        ;;
    "backtest")
        run_backtest
        ;;
    "help"|"-h"|"--help")
        show_help
        ;;
    *)
        # Check if MODE environment variable is set
        case "${MODE:-cron}" in
            "cron")
                start_cron
                ;;
            "live")
                run_live_trend
                ;;
            "backtest")
                run_backtest
                ;;
            *)
                echo -e "${RED}Unknown mode: ${1:-$MODE}${NC}"
                show_help
                exit 1
                ;;
        esac
        ;;
esac
