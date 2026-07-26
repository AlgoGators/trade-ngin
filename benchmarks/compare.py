#!/usr/bin/env python3
"""
Benchmark comparison script: detect performance regressions.

Compares two benchmark JSON files (baseline vs. current) and reports
regressions by percentage. Exits non-zero if any benchmark regresses
beyond the threshold (default 10%).

Handles missing/new benchmarks explicitly - does not crash or silently ignore.

Usage:
    python3 compare.py baseline.json current.json [--threshold 10]
"""

import json
import sys
from pathlib import Path


def load_benchmarks(filepath):
    """Load benchmarks from JSON file. Returns dict {name: {median_ns, p95_ns}}"""
    try:
        with open(filepath, "r") as f:
            data = json.load(f)

        # Handle both array and dict formats
        if isinstance(data, list):
            return {item["name"]: item for item in data}
        else:
            return data
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"Error loading {filepath}: {e}", file=sys.stderr)
        sys.exit(1)


def compare_benchmarks(baseline, current, threshold_percent=10.0):
    """
    Compare benchmarks. Returns (regressions, warnings).

    regressions: list of (name, baseline_ns, current_ns, delta_percent)
    warnings: list of strings describing missing/new benchmarks
    """
    regressions = []
    warnings = []

    # Check for benchmarks in baseline but missing in current
    for name in baseline:
        if name not in current:
            warnings.append(
                f"MISSING in current: {name} (baseline: {baseline[name]['median_ns']} ns)"
            )

    # Check for new benchmarks in current
    for name in current:
        if name not in baseline:
            warnings.append(
                f"NEW in current: {name} (current: {current[name]['median_ns']} ns)"
            )

    # Compare matching benchmarks
    for name in baseline:
        if name not in current:
            continue

        baseline_median = baseline[name]["median_ns"]
        current_median = current[name]["median_ns"]

        # Calculate percentage change: (current - baseline) / baseline * 100
        # Positive = regression (slower), negative = improvement (faster)
        if baseline_median > 0:
            delta_percent = (
                (current_median - baseline_median) / baseline_median
            ) * 100.0
        else:
            delta_percent = 0.0

        # Report if regressed beyond threshold
        if delta_percent > threshold_percent:
            regressions.append((name, baseline_median, current_median, delta_percent))

    return regressions, warnings


def print_results(regressions, warnings, threshold_percent):
    """Print human-readable comparison results."""

    print("\n" + "=" * 80)
    print("Benchmark Comparison Results")
    print("=" * 80 + "\n")

    if warnings:
        print("Warnings:")
        for warning in warnings:
            print(f"  [WARN] {warning}")
        print()

    if regressions:
        print(f"REGRESSIONS DETECTED (threshold: {threshold_percent}%):\n")
        print(f"{'Benchmark':<40} {'Baseline':<15} {'Current':<15} {'Delta %':<10}")
        print("-" * 80)

        for name, baseline_ns, current_ns, delta_percent in regressions:
            print(
                f"{name:<40} {baseline_ns:>14} ns {current_ns:>14} ns {delta_percent:>8.2f}%"
            )

        print("\n" + "=" * 80)
        print(f"FAILED: {len(regressions)} regression(s) detected")
        print("=" * 80 + "\n")
        return False
    else:
        print("[PASS] All benchmarks pass (no regressions beyond threshold)")
        print("=" * 80 + "\n")
        return True


def main():
    if len(sys.argv) < 3:
        print(
            "Usage: python3 compare.py <baseline.json> <current.json> [--threshold <percent>]"
        )
        sys.exit(1)

    baseline_path = sys.argv[1]
    current_path = sys.argv[2]
    threshold_percent = 10.0

    # Parse optional threshold
    if len(sys.argv) > 3:
        if sys.argv[3] == "--threshold" and len(sys.argv) > 4:
            try:
                threshold_percent = float(sys.argv[4])
            except ValueError:
                print(f"Invalid threshold: {sys.argv[4]}", file=sys.stderr)
                sys.exit(1)

    # Load benchmarks
    baseline = load_benchmarks(baseline_path)
    current = load_benchmarks(current_path)

    # Compare
    regressions, warnings = compare_benchmarks(baseline, current, threshold_percent)

    # Print and determine exit code
    passed = print_results(regressions, warnings, threshold_percent)

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
