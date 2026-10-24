#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=$(realpath "${1:-$repo_root/build_coverage}")
lcov_bin=${LCOV:-lcov}

if [[ ! -x "$build_dir/bin/Debug/trade_ngin_tests" ]]; then
    echo "Build the Debug coverage test binary in $build_dir first." >&2
    exit 1
fi
if ! command -v "$lcov_bin" >/dev/null 2>&1; then
    echo "lcov is required (set LCOV to its path if it is outside PATH)." >&2
    exit 1
fi

# The coverage target resets counters, runs CTest serially with an isolated
# TMPDIR, captures the library once, and extracts this checkout's src/.
cmake --build "$build_dir" --target coverage
cp "$build_dir/coverage.info" "$build_dir/cov_full.info"
cp "$build_dir/coverage.info.cleaned" "$build_dir/cov_src.info"
"$lcov_bin" --remove "$build_dir/cov_src.info" \
    '*chart_generator*' '*email_sender*' '*postgres_database*' \
    '*live_trading_coordinator*' '*execution_engine*' \
    --output-file "$build_dir/cov_inscope.info" \
    --ignore-errors unsupported,inconsistent,format,count,unused --quiet

echo "All repository src/ files:"
"$lcov_bin" --summary "$build_dir/cov_src.info" \
    --ignore-errors unsupported,inconsistent,format,count
echo "In scope (five exclusion patterns):"
"$lcov_bin" --summary "$build_dir/cov_inscope.info" \
    --ignore-errors unsupported,inconsistent,format,count
echo "Per-file repository src/ coverage:"
"$lcov_bin" --list "$build_dir/cov_src.info" \
    --ignore-errors unsupported,inconsistent,format,count
