#!/bin/bash

TIMESTAMP=$(date +%Y-%m-%d_%H-%M-%S)
REPORT_FILE="linting/lint_report_$TIMESTAMP.txt"

# Ensure linting directory exists
mkdir -p linting

# Clear previous report
rm -f $REPORT_FILE

echo "Running clang-format..." | tee $REPORT_FILE
clang-format -i src/**/*.cpp include/**/*.hpp

echo "Running cpplint..." | tee -a $REPORT_FILE
cpplint --recursive src include 2>&1 | tee -a $REPORT_FILE

# Check for errors and fail the commit if necessary
if grep -q "error" "$REPORT_FILE"; then
    echo " Linting errors detected. Fix them before committing."
    exit 1
fi

echo " Linting successful. Report saved in $REPORT_FILE."
