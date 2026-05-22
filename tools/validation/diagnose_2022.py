"""
Pinpoint the 2022 -10.20pp alpha leak between Package A and remediation.

Shows:
    - Month-end equity for both runs
    - Per-symbol exposure (notional / equity) at end-of-month
    - Number of crash-override days in each month (inferred from execution
      clusters against the defensive basket signature)
    - Cumulative P&L contribution per symbol in 2022
"""

from __future__ import annotations

import os

import numpy as np
import pandas as pd
import psycopg2
import sys

TIER3 = sys.argv[1] if len(sys.argv) > 1 else "BPGV_ROTATION_20260421_165528_725"
BASELINE = sys.argv[2] if len(sys.argv) > 2 else "BPGV_ROTATION_20260420_235451_768"


def connect():
    return psycopg2.connect(
        host=os.environ["PGHOST"],
        port=int(os.environ.get("PGPORT", 5432)),
        user=os.environ["PGUSER"],
        password=os.environ["PGPASSWORD"],
        dbname=os.environ["PGDATABASE"],
    )


def month_end_equity(conn, run_id: str) -> pd.DataFrame:
    df = pd.read_sql(
        "SELECT timestamp AT TIME ZONE 'UTC' AS date, equity "
        "FROM backtest.equity_curve WHERE run_id = %s ORDER BY timestamp",
        conn, params=(run_id,),
    )
    df["date"] = pd.to_datetime(df["date"])
    df = df.set_index("date").sort_index()
    return df["equity"].resample("ME").last()


def month_end_positions(conn, run_id: str, year: int) -> pd.DataFrame:
    df = pd.read_sql(
        """
        SELECT date, symbol, quantity, average_price,
               unrealized_pnl, realized_pnl
        FROM backtest.final_positions
        WHERE run_id = %s
          AND EXTRACT(YEAR FROM date) = %s
        """,
        conn, params=(run_id, year),
    )
    df["date"] = pd.to_datetime(df["date"])
    return df


def equity_as_of(equity: pd.Series, date) -> float:
    idx = equity.index.asof(date)
    if pd.isna(idx):
        return float("nan")
    return float(equity.loc[idx])


def year_positions_pivot(conn, run_id: str, year: int, equity: pd.Series):
    pos = month_end_positions(conn, run_id, year)
    # Take last observation per (month, symbol).
    pos["month"] = pos["date"].dt.to_period("M")
    last = (
        pos.sort_values("date")
        .groupby(["month", "symbol"], as_index=False)
        .tail(1)
    )
    last["notional"] = (last["quantity"] * last["average_price"]).fillna(0.0)
    # Approximate notional via avg_price (not live price). For an exposure
    # view this slightly under-estimates vs mark-to-market, but differences
    # cancel when comparing two runs on the same day.
    last["equity_approx"] = last["date"].map(lambda d: equity_as_of(equity, d))
    last["exposure_pct"] = 100.0 * last["notional"] / last["equity_approx"]
    pivot = last.pivot_table(
        index="month", columns="symbol", values="exposure_pct",
        aggfunc="last", fill_value=0.0,
    )
    return pivot


def crash_exec_months(conn, run_id: str, year: int):
    df = pd.read_sql(
        """
        SELECT timestamp AT TIME ZONE 'UTC' AS date, symbol, quantity
        FROM backtest.executions
        WHERE run_id = %s
          AND EXTRACT(YEAR FROM timestamp) = %s
        ORDER BY timestamp
        """,
        conn, params=(run_id, year),
    )
    df["date"] = pd.to_datetime(df["date"])
    df["month"] = df["date"].dt.to_period("M")
    return df.groupby("month").size().rename("execs")


def pnl_contribution_2022(conn, run_id: str):
    """
    Approx per-symbol P&L for 2022 = sum_{trades in year} (fill_px - avg_price)
    * quantity. We rely on `executions` which has fill side + qty + price.
    Positions' realized + unrealized PnL deltas YoY are more accurate.
    """
    df = pd.read_sql(
        """
        SELECT symbol,
               SUM(realized_pnl) AS realized_y,
               MAX(unrealized_pnl) - MIN(unrealized_pnl) AS unrealized_range
        FROM backtest.final_positions
        WHERE run_id = %s
          AND EXTRACT(YEAR FROM date) = 2022
        GROUP BY symbol
        ORDER BY realized_y
        """,
        conn, params=(run_id,),
    )
    return df


def main():
    with connect() as conn:
        print("=" * 72)
        print("MONTH-END EQUITY — 2022 comparison")
        print("=" * 72)
        eq_b = month_end_equity(conn, BASELINE)
        eq_t3 = month_end_equity(conn, TIER3)
        cmp = pd.DataFrame({
            "baseline": eq_b,
            "package_A": eq_t3,
        })
        cmp["pkgA_vs_prev_month"] = cmp["package_A"].pct_change() * 100.0
        cmp["base_vs_prev_month"] = cmp["baseline"].pct_change() * 100.0
        cmp["monthly_delta_pp"] = cmp["pkgA_vs_prev_month"] - cmp["base_vs_prev_month"]
        mask = (cmp.index.year >= 2021) & (cmp.index.year <= 2023)
        print(cmp[mask].to_string(float_format=lambda x: f"{x:+10.3f}"))
        print()

        print("=" * 72)
        print("2022 PER-SYMBOL EXPOSURE % BY MONTH — Package A")
        print("=" * 72)
        p_t3 = year_positions_pivot(conn, TIER3, 2022, eq_t3)
        print(p_t3.to_string(float_format=lambda x: f"{x:5.1f}"))
        print()

        print("=" * 72)
        print("2022 PER-SYMBOL EXPOSURE % BY MONTH — Remediation")
        print("=" * 72)
        p_b = year_positions_pivot(conn, BASELINE, 2022, eq_b)
        print(p_b.to_string(float_format=lambda x: f"{x:5.1f}"))
        print()

        print("=" * 72)
        print("2022 P&L PER SYMBOL — Package A (realized, unrealized range)")
        print("=" * 72)
        pnl_t3 = pnl_contribution_2022(conn, TIER3)
        print(pnl_t3.to_string(float_format=lambda x: f"{x:+10.0f}"))
        print()

        print("=" * 72)
        print("2022 P&L PER SYMBOL — Remediation")
        print("=" * 72)
        pnl_b = pnl_contribution_2022(conn, BASELINE)
        print(pnl_b.to_string(float_format=lambda x: f"{x:+10.0f}"))
        print()

        print("=" * 72)
        print("EXECUTIONS PER MONTH 2022")
        print("=" * 72)
        ex_t3 = crash_exec_months(conn, TIER3, 2022)
        ex_b = crash_exec_months(conn, BASELINE, 2022)
        ex_cmp = pd.DataFrame({"pkgA": ex_t3, "base": ex_b}).fillna(0).astype(int)
        print(ex_cmp.to_string())


if __name__ == "__main__":
    main()
