#!/bin/bash

cd /Users/ryanmathieu/Documents/GitHub/trade-ngin
cmake --build build
./build/mock_trading

echo "Daily trading run completed at $(date)" >> trading.log 