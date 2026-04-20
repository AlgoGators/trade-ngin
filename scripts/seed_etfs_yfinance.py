"""
Seed local CSV bars for ETFs that are missing from the Databento-sourced Postgres
tables (equities_data.ohlcv_1d and macro_data.bsts_etf_prices).

These CSVs are consumed at backtest time by the C++ CSVEquityLoader as a fallback
when the DB query returns no rows for a symbol. They are NOT written to Postgres —
the DB is reserved for Databento data only.

Default tickers: QQQ, XLK, SMH, IWM, XHB, IYR (the six ETFs BPGV rotation needs
that aren't in either Postgres table).

Output schema (one file per ticker at data/equity_bars/{TICKER}.csv):
    date,open,high,low,close,adj_close,volume

where `close` is the raw close and `adj_close` is split/dividend adjusted — matches
the semantics of equities_data.ohlcv_1d's `close` / `closeadj` columns.

Usage:
    python scripts/seed_etfs_yfinance.py
    python scripts/seed_etfs_yfinance.py --tickers QQQ XLK SMH
    python scripts/seed_etfs_yfinance.py --output-dir data/equity_bars --start 1999-01-01
"""

import argparse
import os
import sys
import time

import pandas as pd
import yfinance as yf

DEFAULT_TICKERS = ["QQQ", "XLK", "SMH", "IWM", "XHB", "IYR"]
DEFAULT_OUTPUT_DIR = "data/equity_bars"
DEFAULT_START = "1999-01-01"


def download_with_retry(ticker: str, start: str, retries: int = 4, backoff: float = 3.0) -> pd.DataFrame:
    """Download OHLCV for a single ticker with retry/backoff on transient failures."""
    last_err = None
    for attempt in range(1, retries + 1):
        try:
            # auto_adjust=False keeps `Close` as raw close and exposes `Adj Close`
            df = yf.download(
                ticker,
                start=start,
                auto_adjust=False,
                progress=False,
                actions=False,
                threads=False,
            )
            if df is None or df.empty:
                raise RuntimeError(f"yfinance returned empty frame for {ticker}")
            return df
        except Exception as exc:  # noqa: BLE001
            last_err = exc
            wait = backoff * attempt
            print(f"  [{ticker}] attempt {attempt}/{retries} failed: {exc}. Retrying in {wait:.0f}s...")
            time.sleep(wait)
    raise RuntimeError(f"Failed to download {ticker} after {retries} attempts: {last_err}")


def normalize_frame(df: pd.DataFrame, ticker: str) -> pd.DataFrame:
    """Flatten yfinance's potentially-MultiIndex columns into lowercase names."""
    # Modern yfinance returns a MultiIndex (field, ticker) when threads>1 / single-ticker paths collapse.
    if isinstance(df.columns, pd.MultiIndex):
        df.columns = [col[0] if isinstance(col, tuple) else col for col in df.columns]

    expected = {"Open", "High", "Low", "Close", "Adj Close", "Volume"}
    missing = expected - set(df.columns)
    if missing:
        raise RuntimeError(f"{ticker}: missing columns from yfinance: {sorted(missing)}")

    out = pd.DataFrame(
        {
            "date": df.index.strftime("%Y-%m-%d"),
            "open": df["Open"].values,
            "high": df["High"].values,
            "low": df["Low"].values,
            "close": df["Close"].values,
            "adj_close": df["Adj Close"].values,
            "volume": df["Volume"].astype("int64").values,
        }
    )
    # Drop rows where any price field is NaN (yfinance sometimes emits blank holiday rows)
    out = out.dropna(subset=["open", "high", "low", "close", "adj_close"])
    return out


def write_csv(frame: pd.DataFrame, ticker: str, output_dir: str) -> str:
    os.makedirs(output_dir, exist_ok=True)
    path = os.path.join(output_dir, f"{ticker}.csv")
    frame.to_csv(path, index=False, float_format="%.6f")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description="Seed local ETF CSVs from yfinance.")
    parser.add_argument(
        "--tickers",
        nargs="+",
        default=DEFAULT_TICKERS,
        help="Tickers to download (default: QQQ XLK SMH IWM XHB IYR)",
    )
    parser.add_argument(
        "--output-dir",
        default=DEFAULT_OUTPUT_DIR,
        help=f"Directory to write CSVs into (default: {DEFAULT_OUTPUT_DIR})",
    )
    parser.add_argument(
        "--start",
        default=DEFAULT_START,
        help=f"Earliest date to fetch (default: {DEFAULT_START})",
    )
    args = parser.parse_args()

    failures: list[str] = []
    for ticker in args.tickers:
        ticker = ticker.upper()
        print(f"[{ticker}] downloading from {args.start}...")
        try:
            raw = download_with_retry(ticker, args.start)
            frame = normalize_frame(raw, ticker)
            path = write_csv(frame, ticker, args.output_dir)
            first = frame["date"].iloc[0]
            last = frame["date"].iloc[-1]
            print(f"[{ticker}] wrote {len(frame)} rows ({first} -> {last}) to {path}")
        except Exception as exc:  # noqa: BLE001
            print(f"[{ticker}] FAILED: {exc}", file=sys.stderr)
            failures.append(ticker)

    if failures:
        print(f"\n{len(failures)} ticker(s) failed: {', '.join(failures)}", file=sys.stderr)
        return 1
    print(f"\nAll {len(args.tickers)} ticker(s) seeded successfully into {args.output_dir}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
