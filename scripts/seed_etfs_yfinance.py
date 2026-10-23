#!/usr/bin/env python3
"""Seed backtest-only bar CSVs for ETFs the database does not carry yet.

Writes one file per ticker at data/equity_bars/{TICKER}.csv in the engine's equity data
contract -- RAW prices plus the per-bar corporate-action primitives, never a vendor's
adjusted close:

    date,open,high,low,close,volume,div_cash,split_factor

    date          session date (UTC calendar day)
    open..close   raw exchange prices, volume raw shares
    div_cash      cash distribution going ex on that bar (dividends + capital gains), 0 if none
    split_factor  new/old shares taking effect on that bar, 1 if none (Tiingo orientation)

The C++ loader (include/trade_ngin/backtest/csv_bar_source.hpp) adjusts these with the same
backward recurrence the equities_data.ohlcv_1d SQL path uses, so a CSV run and a database
run of the same window are on the same price scale. These files are consumed by backtests
ONLY (DataSource::CSV lives in trade_ngin::backtest); the live path reads the database.

Source: yfinance, `auto_adjust=False, actions=True`. yfinance dates a dividend on its
ex-date bar and a split on its effective bar with the ratio new/old (2.0 for a 2:1, 0.5 for
a 1:2 reverse split), which is the orientation the recurrence divides by, so no inversion.
Yahoo's "Close", "Volume" and "Dividends" are however already SPLIT-adjusted (stated in
current-share units), so this script un-adjusts them first: every bar before a split with
ratio r is multiplied back by r (prices, dividends) and its volume divided by r. What is
written is therefore raw, exactly like equities_data.ohlcv_1d.

--from-db TICKER...: export the same contract straight from equities_data.ohlcv_1d (raw
prices + div_cash + split_factor) through psql for symbols the database carries; the
resulting CSV run then matches a database run bar for bar.

Validation (default on, --no-validate to skip), per ticker:
  1. between corporate-action events the ratio repo_adjusted / yahoo "Adj Close" is constant
     (relative spread < 1e-5): event dates and the anchor line up;
  2. Yahoo's own convention (1 - div / prev_close per ex-date, / split) rebuilt from our
     primitives reproduces "Adj Close" to < 2e-3 relative over the whole file: the primitives
     are complete; a missed dividend or split shows up here immediately;
  3. no raw close move above 30% on a bar with split_factor 1;
  4. weekday-only, strictly ascending dates, no NaN prices.
The two conventions differ by ~1e-5 per dividend (Yahoo divides by the PREVIOUS close, the
engine by close + dividend), so check 2 is a completeness check, not an identity.

Usage:
    python scripts/seed_etfs_yfinance.py                     # the 12 BPGV ETFs
    python scripts/seed_etfs_yfinance.py --tickers SPY TLT   # a subset
    python scripts/seed_etfs_yfinance.py --compare-db EQR    # vs equities_data.ohlcv_1d (needs psycopg2 + PG* env)
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parents[1]

DEFAULT_TICKERS = ["SPY", "QQQ", "XLK", "SMH", "IWM", "XHB", "IYR", "EQR", "TLT", "GLD", "BIL", "DBMF"]
DEFAULT_OUTPUT_DIR = REPO / "data" / "equity_bars"
DEFAULT_START = "1999-01-01"
COLUMNS = ["date", "open", "high", "low", "close", "volume", "div_cash", "split_factor"]


def backward_adjustment_factors(close: np.ndarray, div_cash: np.ndarray,
                                split_factor: np.ndarray) -> np.ndarray:
    """One factor per bar, oldest first, the last bar's factor 1: the recurrence of
    market_data_utils::compute_backward_adjustment_factors, which the engine's CSV and
    database paths both use."""
    factors = np.ones(len(close), dtype=float)
    for i in range(len(close) - 2, -1, -1):
        c, d, s = close[i + 1], div_cash[i + 1], split_factor[i + 1]
        step = 1.0
        if c > 0.0:
            step = (c / (c + d)) / (s if s > 0.0 else 1.0)
        factors[i] = factors[i + 1] * step
    return factors


def backward_adjusted_close(df: pd.DataFrame, anchor_end=None) -> pd.Series:
    """Adjusted close of a COLUMNS frame, anchored at its last bar <= anchor_end."""
    missing = [c for c in COLUMNS if c not in df.columns]
    if missing:
        raise ValueError(f"missing columns {missing}; expected {COLUMNS}")
    d = df if anchor_end is None else df[df["date"] <= pd.Timestamp(anchor_end)]
    d = d.sort_values("date")
    f = backward_adjustment_factors(d["close"].to_numpy(float),
                                    d["div_cash"].fillna(0.0).to_numpy(float),
                                    d["split_factor"].fillna(1.0).to_numpy(float))
    return pd.Series(d["close"].to_numpy(float) * f, index=d.index, name="adj_close")


def download(ticker: str, start: str, retries: int = 4, backoff: float = 5.0) -> pd.DataFrame:
    import yfinance as yf

    last_err = None
    for attempt in range(1, retries + 1):
        try:
            df = yf.Ticker(ticker).history(start=start, auto_adjust=False, actions=True)
            if df is None or df.empty:
                raise RuntimeError(f"yfinance returned an empty frame for {ticker}")
            return df
        except Exception as exc:  # noqa: BLE001
            last_err = exc
            wait = backoff * attempt
            print(f"  [{ticker}] attempt {attempt}/{retries} failed: {exc}; retrying in {wait:.0f}s")
            time.sleep(wait)
    raise RuntimeError(f"failed to download {ticker}: {last_err}")


def last_completed_session(now: pd.Timestamp | None = None) -> pd.Timestamp:
    """Newest date whose regular session has closed in New York (16:00 ET plus 30 minutes
    of slack). While the market is open yfinance returns today's row as a partial bar, which
    must not be written as a completed daily bar."""
    now = pd.Timestamp.now(tz="America/New_York") if now is None else now
    today = now.tz_localize(None).normalize()
    closed = now.hour * 60 + now.minute >= 16 * 60 + 30
    return today if closed else today - pd.Timedelta(days=1)


def to_contract(ticker: str, raw: pd.DataFrame) -> tuple[pd.DataFrame, pd.Series]:
    """Return (contract frame, yahoo adj close aligned to it)."""
    df = raw.copy()
    df.index = pd.to_datetime(df.index).tz_localize(None).normalize()
    needed = {"Open", "High", "Low", "Close", "Adj Close", "Volume"}
    missing = needed - set(df.columns)
    if missing:
        raise RuntimeError(f"{ticker}: yfinance frame lacks {sorted(missing)}")
    div = df["Dividends"] if "Dividends" in df.columns else pd.Series(0.0, index=df.index)
    gains = df["Capital Gains"] if "Capital Gains" in df.columns else pd.Series(0.0, index=df.index)
    splits = df["Stock Splits"] if "Stock Splits" in df.columns else pd.Series(0.0, index=df.index)

    split_factor = np.where(splits.fillna(0.0).to_numpy(float) > 0.0,
                            splits.fillna(0.0).to_numpy(float), 1.0)
    # Un-adjust Yahoo's split-adjusted series back to raw: bars before a split with ratio r
    # were reported in post-split share units, so multiply their prices and dividends by r
    # and divide their volume by r (cumulative over every later split).
    n = len(split_factor)
    back = np.ones(n)
    for i in range(n - 2, -1, -1):
        back[i] = back[i + 1] * split_factor[i + 1]
    out = pd.DataFrame({
        "date": df.index,
        "open": df["Open"].to_numpy(float) * back,
        "high": df["High"].to_numpy(float) * back,
        "low": df["Low"].to_numpy(float) * back,
        "close": df["Close"].to_numpy(float) * back,
        "volume": df["Volume"].fillna(0).to_numpy(float) / back,
        "div_cash": (div.fillna(0.0) + gains.fillna(0.0)).to_numpy(float) * back,
        "split_factor": split_factor,
    })
    yahoo_adj = pd.Series(df["Adj Close"].to_numpy(float), index=out.index)
    keep = out[["open", "high", "low", "close"]].notna().all(axis=1) & yahoo_adj.notna()
    keep &= out["date"] <= last_completed_session()  # never write a partial intraday bar
    out = out[keep].reset_index(drop=True)
    yahoo_adj = yahoo_adj[keep].reset_index(drop=True)
    out = out.sort_values("date").reset_index(drop=True)
    return out, yahoo_adj


def yahoo_convention_adjusted(df: pd.DataFrame) -> np.ndarray:
    """Adj close rebuilt with Yahoo's convention from our primitives."""
    close = df["close"].to_numpy(float)
    div = df["div_cash"].to_numpy(float)
    split = df["split_factor"].to_numpy(float)
    n = len(close)
    f = np.ones(n)
    for i in range(n - 2, -1, -1):
        step = 1.0
        if div[i + 1] > 0.0 and close[i] > 0.0:
            step *= (1.0 - div[i + 1] / close[i])
        if split[i + 1] > 0.0:
            step /= split[i + 1]
        f[i] = f[i + 1] * step
    return close * f


def validate(ticker: str, df: pd.DataFrame, yahoo_adj: pd.Series) -> list[str]:
    problems: list[str] = []
    dates = pd.to_datetime(df["date"])
    if not dates.is_monotonic_increasing or dates.duplicated().any():
        problems.append("dates are not strictly ascending")
    weekend = dates.dt.dayofweek >= 5
    if weekend.any():
        problems.append(f"{int(weekend.sum())} weekend rows")
    if df[["open", "high", "low", "close"]].isna().any().any():
        problems.append("NaN prices")

    close = df["close"].to_numpy(float)
    split = df["split_factor"].to_numpy(float)
    jumps = np.abs(close[1:] / close[:-1] - 1.0)
    bad = (jumps > 0.30) & (split[1:] == 1.0)
    if bad.any():
        idx = np.where(bad)[0] + 1
        problems.append("raw close move > 30% without a split on " +
                        ", ".join(str(df['date'].iloc[i].date()) for i in idx[:5]))

    repo_adj = backward_adjusted_close(df).to_numpy(float)
    yadj = yahoo_adj.to_numpy(float)
    # 1. piecewise-constant ratio between events
    events = (df["div_cash"].to_numpy(float) > 0.0) | (split != 1.0)
    segment = np.cumsum(events)
    worst = 0.0
    for seg in np.unique(segment):
        mask = (segment == seg) & (yadj > 0)
        if mask.sum() < 2:
            continue
        ratio = repo_adj[mask] / yadj[mask]
        spread = ratio.max() / ratio.min() - 1.0
        worst = max(worst, spread)
    if worst > 1e-5:
        problems.append(f"repo/yahoo ratio not constant between events (spread {worst:.2e})")
    # 2. completeness: Yahoo convention from our primitives vs Yahoo Adj Close
    rebuilt = yahoo_convention_adjusted(df)
    ok = yadj > 0
    rel = np.abs(rebuilt[ok] / yadj[ok] - 1.0)
    if rel.max() > 2e-3:
        i = int(np.argmax(rel))
        problems.append(f"primitives do not reproduce Adj Close (max rel diff {rel.max():.2e} at "
                        f"{df['date'][ok].iloc[i].date()})")
    print(f"  [{ticker}] rows={len(df)} {df['date'].iloc[0].date()}..{df['date'].iloc[-1].date()} "
          f"dividends={int((df['div_cash'] > 0).sum())} splits={[(str(d.date()), float(s)) for d, s in zip(df['date'][split != 1.0], split[split != 1.0])]} "
          f"ratio_spread={worst:.1e} adj_rebuild_maxrel={rel.max():.1e}")
    return problems


def compare_db(ticker: str, df: pd.DataFrame) -> None:
    try:
        import psycopg2  # noqa: F401
    except ImportError:
        print(f"  [{ticker}] --compare-db skipped: psycopg2 not installed")
        return
    import psycopg2

    conn = psycopg2.connect(host=os.environ["PGHOST"], port=os.environ.get("PGPORT", "5432"),
                            user=os.environ["PGUSER"], password=os.environ["PGPASSWORD"],
                            dbname=os.environ["PGDATABASE"])
    q = ("SELECT time::date AS date, close, COALESCE(div_cash,0) AS div_cash, "
         "COALESCE(split_factor,1) AS split_factor FROM equities_data.ohlcv_1d "
         "WHERE symbol = %s ORDER BY time")
    db = pd.read_sql(q, conn, params=(ticker,))
    conn.close()
    if db.empty:
        print(f"  [{ticker}] --compare-db: no rows in equities_data.ohlcv_1d")
        return
    db["date"] = pd.to_datetime(db["date"])
    merged = df.merge(db, on="date", suffixes=("_csv", "_db"))
    end = merged["date"].max()
    csv_adj = backward_adjusted_close(df.rename(columns={}), anchor_end=end)
    csv_adj = pd.Series(csv_adj.to_numpy(), index=df[df["date"] <= end].index)
    dbf = db.rename(columns={"close": "close"}).copy()
    dbf["open"] = dbf["high"] = dbf["low"] = dbf["close"]
    dbf["volume"] = 0
    db_adj = backward_adjusted_close(dbf[["date", "open", "high", "low", "close", "volume", "div_cash", "split_factor"]], anchor_end=end)
    a = pd.Series(csv_adj.to_numpy(), index=df[df["date"] <= end]["date"].to_numpy())
    b = pd.Series(db_adj.to_numpy(), index=dbf[dbf["date"] <= end]["date"].to_numpy())
    both = pd.concat([a, b], axis=1, join="inner").dropna()
    rel = np.abs(both.iloc[:, 0] / both.iloc[:, 1] - 1.0)
    print(f"  [{ticker}] --compare-db: {len(both)} common bars, max rel diff {rel.max():.2e}, "
          f"median {rel.median():.2e}; db {db['date'].min().date()}..{db['date'].max().date()}")


def export_from_db(ticker: str, out_dir: Path) -> Path:
    """Write {ticker}.csv from equities_data.ohlcv_1d through psql (PG* env vars)."""
    import subprocess

    sql = ("COPY (SELECT to_char(time AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS date, open, high, low, close, "
           "volume, COALESCE(div_cash, 0) AS div_cash, COALESCE(split_factor, 1) AS split_factor "
           f"FROM equities_data.ohlcv_1d WHERE symbol = '{ticker}' ORDER BY time) TO STDOUT WITH CSV HEADER")
    res = subprocess.run(["psql", "-X", "-c", sql], capture_output=True, text=True, check=True)
    path = out_dir / f"{ticker}.csv"
    df = pd.read_csv(pd.io.common.StringIO(res.stdout))
    if df.empty:
        raise RuntimeError(f"{ticker}: no rows in equities_data.ohlcv_1d")
    df = df.drop_duplicates(subset="date").sort_values("date")
    df["volume"] = df["volume"].fillna(0).round().astype("int64")
    df[COLUMNS].to_csv(path, index=False, float_format="%.6f")
    print(f"  [{ticker}] wrote {path} from equities_data.ohlcv_1d ({len(df)} rows, "
          f"{df['date'].iloc[0]}..{df['date'].iloc[-1]}, dividends={int((df['div_cash'] > 0).sum())})")
    return path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tickers", nargs="+", default=DEFAULT_TICKERS)
    ap.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    ap.add_argument("--start", default=DEFAULT_START)
    ap.add_argument("--no-validate", action="store_true")
    ap.add_argument("--compare-db", nargs="*", default=None, metavar="TICKER")
    ap.add_argument("--from-db", nargs="+", default=[], metavar="TICKER",
                    help="export these tickers from equities_data.ohlcv_1d instead of yfinance")
    ap.add_argument("--sleep", type=float, default=1.5, help="seconds between tickers")
    args = ap.parse_args()

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    failures: dict[str, list[str]] = {}
    for ticker in args.from_db:
        export_from_db(ticker, out_dir)
    yf_tickers = [t for t in args.tickers if t not in set(args.from_db)]
    for i, ticker in enumerate(yf_tickers):
        if i:
            time.sleep(args.sleep)
        print(f"[{ticker}] downloading from {args.start} ...")
        raw = download(ticker, args.start)
        df, yahoo_adj = to_contract(ticker, raw)
        if not args.no_validate:
            problems = validate(ticker, df, yahoo_adj)
            if problems:
                failures[ticker] = problems
                for p in problems:
                    print(f"  [{ticker}] FAIL: {p}")
        path = out_dir / f"{ticker}.csv"
        write = df.copy()
        write["date"] = write["date"].dt.strftime("%Y-%m-%d")
        write["volume"] = write["volume"].round().astype("int64")
        write[COLUMNS].to_csv(path, index=False, float_format="%.6f")
        print(f"  [{ticker}] wrote {path} ({len(write)} rows)")
        if args.compare_db is not None and (not args.compare_db or ticker in args.compare_db):
            compare_db(ticker, df)

    if failures:
        print(f"\nvalidation failed for {sorted(failures)}; files were still written -- inspect before use")
        return 1
    print("\nall tickers validated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
