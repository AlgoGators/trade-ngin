#!/bin/bash

# Automatically fix whitespace and clang-format issues

LINTING_DIR="linting"
mkdir -p $LINTING_DIR

echo "Fixing whitespace issues..."
find src include -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec sed -i '' -E 's/[[:space:]]+$//' {} +

echo "Running clang-format to auto-fix formatting..."
clang-format -i src/**/*.cpp include/**/*.hpp

# Fix common cpplint issues

echo "Fixing two-space comments..."
sed -i '' -E 's/(\S)(\/\/)/\1  \2/' src/**/*.cpp include/**/*.hpp


echo "Fixing long lines by breaking them at 80 characters..."
find src include -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec awk 'length($0) > 80 { print substr($0, 1, 80) "\n" substr($0, 81) } length($0) <= 80 { print }' {} \> {}.tmp \; -exec mv {}.tmp {} \;


echo "Removing unnecessary indentation within namespaces..."
sed -i '' -E '/namespace /,/}/ {s/^    //}' src/**/*.cpp include/**/*.hpp

# Remove redundant blank lines at the start of code blocks
echo "Removing redundant blank lines..."
sed -i '' -E '/{/{N; s/\n[[:space:]]*\n/\n/}' $(find . -name '*.cpp' -o -name '*.h' -o -name '*.hpp')

# Apply clang-format
echo "Running clang-format to auto-fix formatting..."
clang-format -i $(find . -name '*.cpp' -o -name '*.h' -o -name '*.hpp')

echo " Auto-fix completed."
