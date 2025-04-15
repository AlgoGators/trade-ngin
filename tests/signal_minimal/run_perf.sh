#!/bin/bash
set -e
if command -v perf &> /dev/null; then
  perf record -o perf.data --call-graph dwarf ./signal_minimal_test
  perf report -i perf.data > perf_report.txt 2>&1
  echo "perf report written to perf_report.txt"
else
  echo "perf not installed in this container."
fi 