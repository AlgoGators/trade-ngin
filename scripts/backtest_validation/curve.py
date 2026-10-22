"""Strict parsing, prefix causality and scored returns for exported curves."""

import csv
import datetime as dt
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation


class CurveError(ValueError):
    """An export cannot be interpreted safely."""


@dataclass(frozen=True)
class CurveRow:
    run_id: str
    portfolio_id: str
    timestamp: dt.datetime
    equity: Decimal


def _instant(value):
    if not value.endswith("Z"):
        raise CurveError("timestamp must be an explicit UTC instant ending in Z")
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise CurveError("invalid timestamp: " + value) from exc
    if parsed.utcoffset() != dt.timedelta(0):
        raise CurveError("timestamp is not UTC: " + value)
    return parsed


def _display(timestamp):
    return timestamp.strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def read_curve(path, run_id, portfolio_id):
    """Read one unambiguous curve from a SELECT-only CSV export."""
    rows = []
    seen = set()
    with open(path, newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        required = {"run_id", "portfolio_id", "timestamp", "equity"}
        if not required.issubset(reader.fieldnames or []):
            raise CurveError("curve CSV lacks required columns: " + ", ".join(sorted(required)))
        for line, raw in enumerate(reader, start=2):
            if raw["run_id"] != run_id or raw["portfolio_id"] != portfolio_id:
                raise CurveError(f"line {line}: run or portfolio differs from requested export")
            timestamp = _instant(raw["timestamp"])
            if timestamp in seen:
                raise CurveError(f"line {line}: duplicate timestamp {_display(timestamp)}")
            seen.add(timestamp)
            try:
                equity = Decimal(raw["equity"])
            except (InvalidOperation, TypeError) as exc:
                raise CurveError(f"line {line}: invalid equity") from exc
            if not equity.is_finite() or equity <= 0:
                raise CurveError(f"line {line}: equity must be positive and finite")
            rows.append(CurveRow(run_id, portfolio_id, timestamp, equity))
    if not rows:
        raise CurveError("curve export contains no rows")
    if any(left.timestamp >= right.timestamp for left, right in zip(rows, rows[1:])):
        raise CurveError("curve export is not strictly ordered by timestamp")
    return rows


def compare_prefix(short_curve, long_curve, end_date):
    """Compare all stored equity values through inclusive end_date exactly."""
    try:
        end = dt.date.fromisoformat(end_date)
    except ValueError as exc:
        raise CurveError("end_date must be YYYY-MM-DD") from exc
    if {r.portfolio_id for r in short_curve + long_curve} != {short_curve[0].portfolio_id}:
        raise CurveError("curves have different portfolio IDs")
    short = {r.timestamp: r.equity for r in short_curve if r.timestamp.date() <= end}
    long = {r.timestamp: r.equity for r in long_curve if r.timestamp.date() <= end}
    if not short or not long:
        raise CurveError("no shared prefix to compare")
    result = {"status": "pass", "short_run": short_curve[0].run_id,
              "long_run": long_curve[0].run_id, "portfolio_id": short_curve[0].portfolio_id,
              "end_date": end_date, "short_rows": len(short), "long_rows": len(long),
              "matched_rows": 0, "first_difference": None}
    for timestamp in sorted(short.keys() | long.keys()):
        a, b = short.get(timestamp), long.get(timestamp)
        if a != b:
            result["status"] = "fail"
            result["first_difference"] = {
                "timestamp": _display(timestamp),
                "short_equity": str(a) if a is not None else None,
                "long_equity": str(b) if b is not None else None,
            }
            return result
        result["matched_rows"] += 1
    return result


def scored_returns(curve, warmup_last_timestamp, scored_start, scored_end):
    """Use the documented last warm-up row as the first scored return's base."""
    base_time = _instant(warmup_last_timestamp)
    try:
        start = dt.date.fromisoformat(scored_start)
        end = dt.date.fromisoformat(scored_end)
    except ValueError as exc:
        raise CurveError("scored dates must be YYYY-MM-DD") from exc
    if start > end:
        raise CurveError("scored_start exceeds scored_end")
    base_indices = [i for i, row in enumerate(curve) if row.timestamp == base_time]
    if len(base_indices) != 1:
        raise CurveError("documented warm-up base is absent or duplicated")
    base_index = base_indices[0]
    if base_index + 1 >= len(curve) or curve[base_index + 1].timestamp.date() != start:
        raise CurveError("last warm-up row must immediately precede first scored row")
    if base_time.date() >= start:
        raise CurveError("warm-up base must precede scored start")
    result = []
    previous = curve[base_index]
    for row in curve[base_index + 1:]:
        if row.timestamp.date() > end:
            break
        if row.timestamp.date() < start:
            raise CurveError("unscored row lies between warm-up base and scored start")
        result.append((row.timestamp, float(row.equity / previous.equity - 1)))
        previous = row
    if not result or result[-1][0].date() != end:
        raise CurveError("scored end date is absent from curve")
    return result
