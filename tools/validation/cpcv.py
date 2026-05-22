"""
Combinatorial Purged K-Fold Cross-Validation (Lopez de Prado 2018
Ch. 12) — fold generator only. The actual CV loop runs via runner.py,
since each fold requires re-running the C++ backtest binary on the
restricted date range.

CPCV settings for BPGV (15y = 180 months):
    N = 10 groups of 18 months each.
    k = 2 test groups per split => C(10, 2) = 45 splits.
    Purge gap = 12 months (the longest strategy lookback).
    Embargo = 3 months (post-test cooling-off).

Each split yields a train-set date range (mutable, with purge) and a
test-set date range (immutable). The runner produces 9 OOS equity
curves by reconstruction (each month appears in exactly k/N = 20 % of
test sets).
"""

from __future__ import annotations

from dataclasses import dataclass
from itertools import combinations
from typing import Iterator

import pandas as pd


@dataclass
class CPCVSplit:
    """One CPCV fold."""

    split_idx: int
    train_ranges: list[tuple[pd.Timestamp, pd.Timestamp]]
    test_ranges: list[tuple[pd.Timestamp, pd.Timestamp]]
    purged_months: int
    embargo_months: int


def make_cpcv_splits(
    start: pd.Timestamp,
    end: pd.Timestamp,
    n_groups: int = 10,
    k_test: int = 2,
    purge_months: int = 12,
    embargo_months: int = 3,
) -> Iterator[CPCVSplit]:
    """
    Generate all C(N, k) CPCV splits over `start`..`end`.

    Each split's train set excludes:
        (a) the k_test test groups themselves, and
        (b) any train month within `purge_months` of a test month OR
            within `embargo_months` after a test group (monthly units).
    """
    # Partition the full window into N contiguous month-groups of roughly
    # equal length. Month units.
    month_starts = pd.date_range(start=start.to_period("M").to_timestamp(),
                                 end=end.to_period("M").to_timestamp(),
                                 freq="MS")
    total_months = len(month_starts)
    if total_months < n_groups * 2:
        raise ValueError(
            f"Not enough months ({total_months}) for {n_groups} groups"
        )

    # Group boundaries in month indices [0..total_months).
    bounds = []
    for g in range(n_groups):
        lo = int(round(g * total_months / n_groups))
        hi = int(round((g + 1) * total_months / n_groups))
        bounds.append((lo, hi - 1))  # inclusive

    split_idx = 0
    for test_group_combo in combinations(range(n_groups), k_test):
        # Build month-index sets.
        test_months = set()
        test_ranges = []
        for gi in test_group_combo:
            lo, hi = bounds[gi]
            test_months.update(range(lo, hi + 1))
            test_ranges.append((month_starts[lo], month_starts[hi]))

        # Purge = ± purge_months around each test month.
        purged = set()
        for m in test_months:
            for d in range(-purge_months, purge_months + 1):
                if 0 <= m + d < total_months:
                    purged.add(m + d)

        # Embargo = post-test cooling off (forward only).
        for m in test_months:
            for d in range(1, embargo_months + 1):
                if m + d < total_months:
                    purged.add(m + d)

        # Train months = everything not in test ∪ purged.
        train_mask = set(range(total_months)) - test_months - purged

        # Convert the train set to contiguous ranges.
        train_ranges = _mask_to_ranges(sorted(train_mask), month_starts)

        yield CPCVSplit(
            split_idx=split_idx,
            train_ranges=train_ranges,
            test_ranges=test_ranges,
            purged_months=purge_months,
            embargo_months=embargo_months,
        )
        split_idx += 1


def _mask_to_ranges(
    sorted_indices: list[int],
    month_starts: pd.DatetimeIndex,
) -> list[tuple[pd.Timestamp, pd.Timestamp]]:
    if not sorted_indices:
        return []
    ranges = []
    run_start = sorted_indices[0]
    prev = run_start
    for idx in sorted_indices[1:]:
        if idx == prev + 1:
            prev = idx
            continue
        ranges.append((month_starts[run_start], month_starts[prev]))
        run_start = idx
        prev = idx
    ranges.append((month_starts[run_start], month_starts[prev]))
    return ranges


if __name__ == "__main__":
    start = pd.Timestamp("2011-01-01")
    end = pd.Timestamp("2026-01-01")
    splits = list(make_cpcv_splits(start, end))
    print(f"Generated {len(splits)} CPCV splits")
    # Show first 3.
    for s in splits[:3]:
        print(f"\nSplit {s.split_idx}:")
        print(f"  test  ranges: {[(a.date(), b.date()) for a, b in s.test_ranges]}")
        print(f"  train ranges: {len(s.train_ranges)} contiguous blocks")
        for a, b in s.train_ranges:
            print(f"    {a.date()} .. {b.date()}")
