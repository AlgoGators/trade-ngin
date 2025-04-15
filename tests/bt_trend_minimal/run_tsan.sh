#!/bin/bash
set -e
TSAN_OPTIONS="suppressions=/app/tsan.supp" ./bt_trend_minimal_test 2>&1 | tee tsan.log 