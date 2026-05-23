"""
Load benchmark return series for Ledoit-Wolf pairwise Sharpe tests:
    - SPY (100 % equity)
    - 60/40 SPY/TLT
    - Equal-weight SPY / TLT / GLD

Sources the same `data/equity_bars/<SYMBOL>.csv` files produced by
`scripts/seed_etfs_yfinance.py`. Returns are computed as pandas
Series aligned by date.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pandas as pd


BARS_DIR = Path(__file__).resolve().parents[2] / "data" / "equity_bars"


def load_prices(symbol: str) -> pd.Series:
    """
    Load adjusted close series for a symbol. Returns pd.Series indexed by
    date with float close values.
    """
    path = BARS_DIR / f"{symbol}.csv"
    if not path.exists():
        raise FileNotFoundError(f"No bars file for {symbol} at {path}")
    df = pd.read_csv(path, parse_dates=["date"])
    df = df.set_index("date").sort_index()
    # Prefer the dividend-adjusted (total-return) close: the C++ backtest trades
    # adj_close (csv_equity_loader.cpp applies the adj_close/close ratio), so the
    # benchmarks must be total-return too or the comparison is biased ~1.5%/yr in
    # the strategy's favor. Raw `close` is only a last-resort fallback.
    for col in ("adj_close", "adjusted_close", "close", "Close"):
        if col in df.columns:
            return df[col].astype(float).rename(symbol)
    raise KeyError(f"No close column in {path}; have {df.columns.tolist()}")


def daily_returns(symbol: str) -> pd.Series:
    return load_prices(symbol).pct_change().dropna()


def benchmark_returns(
    start: pd.Timestamp | None = None,
    end: pd.Timestamp | None = None,
) -> pd.DataFrame:
    """
    Return a DataFrame with columns: spy, sixty_forty, ew_spy_tlt_gld.
    Daily returns over [start, end], inner-joined on overlapping dates.
    """
    spy = daily_returns("SPY")
    tlt = daily_returns("TLT")
    gld = daily_returns("GLD")

    df = pd.concat([spy.rename("spy"),
                    tlt.rename("tlt"),
                    gld.rename("gld")], axis=1).dropna()

    df["sixty_forty"] = 0.60 * df["spy"] + 0.40 * df["tlt"]
    df["ew_spy_tlt_gld"] = (df["spy"] + df["tlt"] + df["gld"]) / 3.0

    if start is not None:
        df = df[df.index >= pd.Timestamp(start)]
    if end is not None:
        df = df[df.index <= pd.Timestamp(end)]
    return df[["spy", "sixty_forty", "ew_spy_tlt_gld"]]


if __name__ == "__main__":
    bmk = benchmark_returns(start=pd.Timestamp("2011-01-01"),
                            end=pd.Timestamp("2026-01-01"))
    print(bmk.head())
    print(bmk.describe())
