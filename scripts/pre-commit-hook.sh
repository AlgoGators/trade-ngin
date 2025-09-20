#!/bin/bash

# Pre-commit hook script for local linting checks
# This script should be run before committing code

set -e

echo "🔍 Running pre-commit checks..."

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Check if required tools are installed
echo "📋 Checking required tools..."

if ! command_exists clang-format; then
    echo -e "${RED}❌ clang-format is not installed. Please install it first.${NC}"
    echo "   On Ubuntu/Debian: sudo apt-get install clang-format"
    echo "   On macOS: brew install clang-format"
    exit 1
fi

if ! command_exists cpplint; then
    echo -e "${RED}❌ cpplint is not installed. Please install it first.${NC}"
    echo "   On Ubuntu/Debian: sudo apt-get install cpplint"
    echo "   On macOS: pip install cpplint"
    exit 1
fi

echo -e "${GREEN}✅ All required tools are installed${NC}"

# Create linting directory if it doesn't exist
mkdir -p linting

# Generate timestamp for report
TIMESTAMP=$(date +%Y-%m-%d_%H-%M-%S)
REPORT_FILE="linting/pre_commit_report_$TIMESTAMP.txt"

echo "📝 Creating linting report: $REPORT_FILE"

# Clear previous report
rm -f $REPORT_FILE

echo "Linting Report - $(date)" > $REPORT_FILE
echo "=================================" >> $REPORT_FILE

# Check code formatting
echo "🔧 Checking code formatting..."
echo "Clang Format Check:" >> $REPORT_FILE

FORMAT_ISSUES=0
for file in $(find src include -name "*.cpp" -o -name "*.hpp"); do
    if ! clang-format --dry-run --Werror "$file" >/dev/null 2>&1; then
        echo -e "${YELLOW}⚠️  Formatting issues found in: $file${NC}"
        echo "Formatting issues in: $file" >> $REPORT_FILE
        FORMAT_ISSUES=$((FORMAT_ISSUES + 1))
    fi
done

if [ $FORMAT_ISSUES -eq 0 ]; then
    echo -e "${GREEN}✅ Code formatting is correct${NC}"
    echo "No formatting issues found" >> $REPORT_FILE
else
    echo -e "${RED}❌ Found $FORMAT_ISSUES files with formatting issues${NC}"
    echo "Found $FORMAT_ISSUES files with formatting issues" >> $REPORT_FILE
fi

echo "" >> $REPORT_FILE
echo "Cpplint Results:" >> $REPORT_FILE

# Run cpplint
echo "🔍 Running cpplint..."
CPPLINT_OUTPUT=$(cpplint --recursive --filter=-legal/copyright,-build/include_order src include 2>&1 || true)
echo "$CPPLINT_OUTPUT" >> $REPORT_FILE

# Count cpplint issues
CPPLINT_ISSUES=$(echo "$CPPLINT_OUTPUT" | grep -c "Total errors found" || echo "0")

if [ "$CPPLINT_ISSUES" -eq 0 ] || echo "$CPPLINT_OUTPUT" | grep -q "Done processing"; then
    echo -e "${GREEN}✅ Cpplint completed${NC}"
else
    echo -e "${YELLOW}⚠️  Cpplint found some issues (see report for details)${NC}"
fi

# Check for common issues
echo "🔍 Running additional checks..."

# Check for TODO comments
TODO_COUNT=$(grep -r "TODO" src include | wc -l || echo "0")
if [ "$TODO_COUNT" -gt 0 ]; then
    echo -e "${YELLOW}⚠️  Found $TODO_COUNT TODO comments${NC}"
    echo "Found $TODO_COUNT TODO comments" >> $REPORT_FILE
fi

# Check for FIXME comments
FIXME_COUNT=$(grep -r "FIXME" src include | wc -l || echo "0")
if [ "$FIXME_COUNT" -gt 0 ]; then
    echo -e "${YELLOW}⚠️  Found $FIXME_COUNT FIXME comments${NC}"
    echo "Found $FIXME_COUNT FIXME comments" >> $REPORT_FILE
fi

# Check for potential memory leaks
MEMORY_ISSUES=$(grep -r "new.*delete\|malloc.*free" src include | wc -l || echo "0")
if [ "$MEMORY_ISSUES" -gt 0 ]; then
    echo -e "${YELLOW}⚠️  Found potential manual memory management${NC}"
    echo "Found potential manual memory management" >> $REPORT_FILE
fi

# Summary
echo "" >> $REPORT_FILE
echo "Summary:" >> $REPORT_FILE
echo "- Formatting issues: $FORMAT_ISSUES" >> $REPORT_FILE
echo "- Cpplint issues: $CPPLINT_ISSUES" >> $REPORT_FILE
echo "- TODO comments: $TODO_COUNT" >> $REPORT_FILE
echo "- FIXME comments: $FIXME_COUNT" >> $REPORT_FILE

# Final decision
if [ $FORMAT_ISSUES -eq 0 ]; then
    echo -e "${GREEN}✅ Pre-commit checks passed!${NC}"
    echo "Pre-commit checks passed" >> $REPORT_FILE
    exit 0
else
    echo -e "${RED}❌ Pre-commit checks failed due to formatting issues${NC}"
    echo "Pre-commit checks failed" >> $REPORT_FILE
    echo ""
    echo "To fix formatting issues, run:"
    echo "  ./linting/auto_fix_lint.sh"
    echo ""
    echo "Or manually fix the formatting issues and run this script again."
    exit 1
fi
