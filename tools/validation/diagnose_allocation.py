"""
Ad-hoc diagnostic: compare Package A run to remediation baseline on:
    - per-year return
    - per-symbol exposure over time (avg notional / portfolio value)
    - drawdown periods
    - crash override dates

Usage:
    PGHOST=... PGPORT=... PGUSER=... PGPASSWORD=... PGDATABASE=... \
        python tools/validation/diagnose_allocation.py \
            --tier3 BPGV_ROTATION_20260421_165528_725 \
            --baseline BPGV_ROTATION_20260420_235451_768
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np
import pandas as pd
import psycopg2


def connect():
    return psycopg2.connect(
        host=os.environ["PGHOST"],
        port=int(os.environ.get("PGPORT", 5432)),
        user=os.environ["PGUSER"],
        password=os.environ["PGPASSWORD"],
        dbname=os.environ["PGDATABASE"],
    )


def equity_curve(conn, run_id: str) -> pd.Series:
    df = pd.read_sql(
        "SELECT timestamp AT TIME ZONE 'UTC' AS date, equity "
        "FROM backtest.equity_curve WHERE run_id = %s ORDER BY timestamp",
        conn, params=(run_id,),
    )
    df["date"] = pd.to_datetime(df["date"])
    return df.set_index("date")["equity"].astype(float)


def yearly_returns(equity: pd.Series) -> pd.Series:
    year_end = equity.resample("YE").last()
    # First-month prior fill
    first = equity.resample("YS").first()
    combined = pd.DataFrame({"start": first.values, "end": year_end.values},
                            index=year_end.index.year)
    return (combined["end"] / combined["start"] - 1.0) * 100.0


def avg_exposure(conn, run_id: str) -> pd.DataFrame:
    """
    Average per-symbol exposure (position * price / portfolio_value) per year.
    Uses backtest.daily_positions which stores (symbol, quantity, close, date)
    plus the equity_curve for denominator.
    """
    # Check table names — try a couple.
    for table in ("backtest.daily_positions", "backtest.positions",
                  "backtest.final_positions"):
        try:
            df = pd.read_sql(
                f"SELECT date, symbol, quantity, price, notional "
                f"FROM {table} WHERE run_id = %s",
                conn, params=(run_id,),
            )
            print(f"  Using table {table}", file=sys.stderr)
            break
        except Exception:
            continue
    else:
        raise RuntimeError("No positions table found")

    df["date"] = pd.to_datetime(df["date"])
    eq = pd.read_sql(
        "SELECT timestamp AT TIME ZONE 'UTC' AS date, equity "
        "FROM backtest.equity_curve WHERE run_id = %s",
        conn, params=(run_id,),
    )
    eq["date"] = pd.to_datetime(eq["date"])
    eq = eq.set_index("date")["equity"].astype(float)

    df["portfolio_value"] = df["date"].map(lambda d: eq.asof(d) if d in eq.index else np.nan)
    df = df.dropna(subset=["portfolio_value"])
    df["exposure"] = df["notional"].astype(float) / df["portfolio_value"]
    df["year"] = df["date"].dt.year
    return (df.groupby(["year", "symbol"])["exposure"].mean().unstack(fill_value=0.0)
            * 100.0)  # percent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tier3", required=True)
    parser.add_argument("--baseline", required=True)
    args = parser.parse_args()

    with connect() as conn:
        print("==== Yearly returns (%) ====")
        yr_t3 = yearly_returns(equity_curve(conn, args.tier3))
        yr_b = yearly_returns(equity_curve(conn, args.baseline))
        comparison = pd.DataFrame({
            "remediation": yr_b,
            "package_A": yr_t3,
            "delta_pp": yr_t3 - yr_b,
        })
        print(comparison.to_string(float_format=lambda x: f"{x:+7.2f}"))
        print()

        print("==== Avg per-symbol exposure by year (%) — Package A ====")
        exp_t3 = avg_exposure(conn, args.tier3)
        print(exp_t3.to_string(float_format=lambda x: f"{x:5.1f}"))
        print()

        print("==== Avg per-symbol exposure by year (%) — Remediation ====")
        exp_b = avg_exposure(conn, args.baseline)
        print(exp_b.to_string(float_format=lambda x: f"{x:5.1f}"))
        print()

        print("==== Exposure DELTA (Package A − Remediation, pp) ====")
        # Align columns.
        all_cols = sorted(set(exp_t3.columns) | set(exp_b.columns))
        for c in all_cols:
            if c not in exp_t3:
                exp_t3[c] = 0.0
            if c not in exp_b:
                exp_b[c] = 0.0
        delta = exp_t3[all_cols] - exp_b[all_cols]
        print(delta.to_string(float_format=lambda x: f"{x:+5.1f}"))


if __name__ == "__main__":
    main()
