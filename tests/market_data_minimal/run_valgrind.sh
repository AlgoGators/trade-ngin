#!/bin/bash
set -e
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./market_data_minimal_test 2>&1 | tee valgrind.log 