"""
download_weekly_macro_data.py
AlgoGators Investment Fund — Quantitative Research

Downloads weekly data for the BSTS macro regime detector:
  - 8 ETF proxies via yfinance  (market momentum signals)
  - 12 FRED macro series        (fundamental regime drivers)

All series are aligned to a weekly Friday grid via forward-fill.
Monthly/quarterly FRED releases hold their last known value until
the next release — economically correct (you don't know the new
number until it's published).

Usage:
    python scripts/download_weekly_macro_data.py
    python scripts/download_weekly_macro_data.py \
        --start 2014-01-01 \
        --end   2026-03-21 \
        --output assets/weekly_macro_prices.csv \
        --fred-key YOUR_KEY

    # Or set env var:
    export FRED_API_KEY=your_key
    python scripts/download_weekly_macro_data.py

Requirements:
    pip install pandas yfinance requests

Output CSV format (one row per Friday):
    date,SPY,EEM,TLT,HYG,GLD,UUP,USO,CPER,
         cpi_yoy,pce_yoy,breakeven_10y,
         gdp_qoq,ism_pmi,indpro_yoy,
         t10y2y,real_rate_10y,hy_oas,
         unrate,payrolls_mom,init_claims

FRED series used:
    Inflation : CPIAUCSL  (monthly → YoY % chg)
                PCEPI     (monthly → YoY % chg)
                T10YIE    (daily   → level, 10Y breakeven)
    Growth    : GDPC1     (quarterly → QoQ % chg annualised)
                NAPM      (monthly → level, ISM PMI)
                INDPRO    (monthly → YoY % chg)
    Yield curve: T10Y2Y   (daily   → level, 10Y-2Y spread)
                 DFII10   (daily   → level, 10Y real rate)
                 BAMLH0A0HYM2 (daily → level, HY OAS)
    Labor     : UNRATE    (monthly → level)
                PAYEMS    (monthly → MoM % chg)
                ICSA      (weekly  → level, initial claims)
"""

from __future__ import annotations

import argparse
import os
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import pandas as pd
import requests
import yfinance as yf


# ─────────────────────────────────────────────────────────────────────────────
# CONFIG
# ─────────────────────────────────────────────────────────────────────────────

ETF_TICKERS: List[str] = ["SPY", "EEM", "TLT", "HYG", "GLD", "UUP", "USO", "CPER"]

# (fred_series_id, output_column_name, transform)
# transform: "level" | "yoy" | "mom" | "qoq_ann"
FRED_SERIES: List[Tuple[str, str, str]] = [
    # Inflation
    ("CPIAUCSL",       "cpi_yoy",       "yoy"),
    ("PCEPI",          "pce_yoy",       "yoy"),
    ("T10YIE",         "breakeven_10y", "level"),
    # Growth
    ("GDPC1",          "gdp_qoq",       "qoq_ann"),
    ("CFNAI",          "cfnai",         "level"),   # Chicago Fed NAI — replaces NAPM (retired)
    ("INDPRO",         "indpro_yoy",    "yoy"),
    # Yield curve
    ("T10Y2Y",         "t10y2y",        "level"),
    ("DFII10",         "real_rate_10y", "level"),
    ("BAMLH0A0HYM2",   "hy_oas",        "level"),
    # Labor
    ("UNRATE",         "unrate",        "level"),
    ("PAYEMS",         "payrolls_mom",  "mom"),
    ("ICSA",           "init_claims",   "level"),
]

FRED_BASE = "https://api.stlouisfed.org/fred/series/observations"
WEEKLY_RULE = "W-FRI"
REQUEST_DELAY = 0.25  # seconds between FRED calls to avoid rate limiting


# ─────────────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class Config:
    start: str = "2014-01-01"
    end: str   = "2026-03-21"
    output: str = "assets/weekly_macro_prices.csv"
    fred_key: str = ""


def parse_args() -> Config:
    parser = argparse.ArgumentParser(
        description="Download weekly ETF + macro data for BSTS regime detector."
    )
    parser.add_argument("--start",    default="2014-01-01")
    parser.add_argument("--end",      default="2026-03-21")
    parser.add_argument("--output",   default="assets/weekly_macro_prices.csv")
    parser.add_argument("--fred-key", default="", dest="fred_key",
                        help="FRED API key (or set FRED_API_KEY env var)")
    args = parser.parse_args()

    key = args.fred_key or os.environ.get("FRED_API_KEY", "")
    return Config(start=args.start, end=args.end, output=args.output, fred_key=key)


# ─────────────────────────────────────────────────────────────────────────────
# ETF DOWNLOAD
# ─────────────────────────────────────────────────────────────────────────────

def download_etf_prices(tickers: List[str], start: str, end: str) -> pd.DataFrame:
    """Download adjusted weekly close prices for ETF universe."""
    print(f"  Downloading {len(tickers)} ETF tickers via yfinance...")
    raw = yf.download(
        tickers=tickers,
        start=start,
        end=end,
        auto_adjust=True,
        progress=False,
        group_by="column",
        threads=True,
    )
    if raw.empty:
        raise ValueError("yfinance returned no data.")

    close = raw["Close"] if isinstance(raw.columns, pd.MultiIndex) else raw[["Close"]]
    if isinstance(close, pd.Series):
        close = close.to_frame(name=tickers[0])

    close = close.sort_index()

    missing = [t for t in tickers if t not in close.columns]
    if missing:
        raise ValueError(f"Missing tickers in yfinance response: {missing}")

    # Resample to weekly Friday, forward-fill gaps
    weekly = close[tickers].resample(WEEKLY_RULE).last().ffill().bfill()
    print(f"  ETF: {len(weekly)} weekly rows, {weekly.isna().sum().sum()} NaNs")
    return weekly


# ─────────────────────────────────────────────────────────────────────────────
# FRED DOWNLOAD + TRANSFORMS
# ─────────────────────────────────────────────────────────────────────────────

def fetch_fred_series(series_id: str, start: str, end: str, api_key: str) -> pd.Series:
    """Fetch a single FRED series as a dated pandas Series."""
    params = {
        "series_id":          series_id,
        "observation_start":  start,
        "observation_end":    end,
        "api_key":            api_key,
        "file_type":          "json",
    }
    resp = requests.get(FRED_BASE, params=params, timeout=20)
    resp.raise_for_status()
    obs = resp.json().get("observations", [])

    records = {}
    for o in obs:
        if o["value"] != ".":
            try:
                records[pd.Timestamp(o["date"])] = float(o["value"])
            except (ValueError, KeyError):
                pass

    if not records:
        raise ValueError(f"No data returned for FRED series {series_id}")

    s = pd.Series(records, name=series_id).sort_index()
    return s


def apply_transform(s: pd.Series, transform: str) -> pd.Series:
    """
    Apply the requested percentage-change or level transform.
    Uses integer-lag shifting based on detected data frequency.
    Robust across pandas versions — avoids freq-based shift which
    changed behaviour in pandas 2.x.
    """
    if transform == "level":
        return s

    if len(s) < 2:
        return s

    # Detect data frequency from median gap between observations (in days)
    gaps = pd.Series(s.index).diff().dropna().dt.days
    median_gap = float(gaps.median())

    if transform == "yoy":
        if median_gap <= 1:
            lag = 252    # daily  → ~1 trading year
        elif median_gap <= 10:
            lag = 52     # weekly → 52 weeks
        elif median_gap <= 35:
            lag = 12     # monthly → 12 months
        else:
            lag = 4      # quarterly → 4 quarters
        prev = s.shift(lag)
        return ((s - prev) / prev.abs() * 100.0).replace([float("inf"), float("-inf")], float("nan"))

    elif transform == "mom":
        if median_gap <= 1:
            lag = 21     # daily  → ~1 trading month
        elif median_gap <= 10:
            lag = 4      # weekly → 4 weeks
        elif median_gap <= 35:
            lag = 1      # monthly → 1 month
        else:
            lag = 1      # quarterly → treat as QoQ
        prev = s.shift(lag)
        return ((s - prev) / prev.abs() * 100.0).replace([float("inf"), float("-inf")], float("nan"))

    elif transform == "qoq_ann":
        if median_gap <= 35:
            lag = 3      # monthly data → 3 months = 1 quarter
        else:
            lag = 1      # quarterly data → 1 period
        prev = s.shift(lag)
        qoq = ((s / prev) ** 4 - 1.0) * 100.0
        return qoq.replace([float("inf"), float("-inf")], float("nan"))

    else:
        raise ValueError(f"Unknown transform: {transform}")


def download_fred_series(
    series_list: List[Tuple[str, str, str]],
    start: str,
    end: str,
    api_key: str,
    weekly_index: pd.DatetimeIndex,
) -> pd.DataFrame:
    """
    Download all FRED series, apply transforms, align to weekly grid.
    Returns DataFrame with one column per series, indexed by weekly Fridays.
    """
    if not api_key:
        raise ValueError(
            "FRED API key required. Get one free at https://fred.stlouisfed.org/docs/api/api_key.html\n"
            "Then set --fred-key or export FRED_API_KEY=your_key"
        )

    result = pd.DataFrame(index=weekly_index)

    for series_id, col_name, transform in series_list:
        print(f"  FRED {series_id:20s} -> {col_name:20s} ({transform})")
        try:
            raw = fetch_fred_series(series_id, start, end, api_key)
            transformed = apply_transform(raw, transform)

            # Drop NaNs from transform edges before reindexing
            transformed = transformed.dropna()

            # Reindex to weekly grid: forward-fill (hold last known value)
            # First expand to daily, then resample to weekly
            daily = transformed.resample("D").last().ffill()
            weekly_col = daily.reindex(weekly_index, method="ffill")

            result[col_name] = weekly_col
            n_valid = result[col_name].notna().sum()
            print(f"    {n_valid}/{len(weekly_index)} weeks populated")

        except Exception as e:
            print(f"    WARNING: Failed to fetch {series_id}: {e}")
            result[col_name] = float("nan")

        time.sleep(REQUEST_DELAY)

    return result


# ─────────────────────────────────────────────────────────────────────────────
# ASSEMBLY + VALIDATION
# ─────────────────────────────────────────────────────────────────────────────

def assemble_dataset(
    etf: pd.DataFrame,
    macro: pd.DataFrame,
) -> pd.DataFrame:
    """
    Join ETF prices and macro series on the weekly Friday index.
    ETF prices are kept as raw adjusted-close (C++ model takes log internally).
    Macro series are kept as transformed levels/changes.
    """
    combined = etf.join(macro, how="left")

    # Forward-fill any remaining gaps (e.g. FRED series starts later than ETF)
    combined = combined.ffill().bfill()

    # Reset index, rename to 'date'
    combined = combined.reset_index()
    combined = combined.rename(columns={"index": "date", "Date": "date", "Datetime": "date"})
    if combined.columns[0] != "date":
        combined = combined.rename(columns={combined.columns[0]: "date"})

    combined["date"] = pd.to_datetime(combined["date"]).dt.strftime("%Y-%m-%d")
    return combined


def validate_dataset(df: pd.DataFrame) -> None:
    """Basic sanity checks on the assembled dataset."""
    etf_cols = ETF_TICKERS
    macro_cols = [col for _, col, _ in FRED_SERIES]
    all_expected = ["date"] + etf_cols + macro_cols

    missing_cols = [c for c in all_expected if c not in df.columns]
    if missing_cols:
        raise ValueError(f"Missing columns in output: {missing_cols}")

    # ETF prices must be positive and finite
    for col in etf_cols:
        bad = (~df[col].apply(lambda x: isinstance(x, float) and x > 0 and x == x)).sum()
        if bad > 0:
            raise ValueError(f"ETF column {col} has {bad} invalid values")

    # Macro series: warn on high NaN rate but don't hard-fail
    # (some series have publication lags at the start of the sample)
    for col in macro_cols:
        nan_rate = df[col].isna().mean()
        if nan_rate > 0.30:
            print(f"  WARNING: {col} is {nan_rate:.0%} NaN — check FRED availability")

    print(f"  Validation passed: {len(df)} rows, {len(df.columns)-1} series")


# ─────────────────────────────────────────────────────────────────────────────
# MAIN
# ─────────────────────────────────────────────────────────────────────────────

def main() -> None:
    cfg = parse_args()

    print("=" * 60)
    print("  BSTS Macro Regime — Data Download")
    print(f"  Period : {cfg.start} → {cfg.end}")
    print(f"  Output : {cfg.output}")
    print("=" * 60)

    # 1. ETF prices
    print("\n[1/3] ETF prices (yfinance)")
    etf_weekly = download_etf_prices(ETF_TICKERS, cfg.start, cfg.end)

    # 2. FRED macro series
    print("\n[2/3] FRED macro series")
    macro_weekly = download_fred_series(
        FRED_SERIES,
        start=cfg.start,
        end=cfg.end,
        api_key=cfg.fred_key,
        weekly_index=etf_weekly.index,
    )

    # 3. Assemble + validate + save
    print("\n[3/3] Assembling dataset")
    dataset = assemble_dataset(etf_weekly, macro_weekly)
    validate_dataset(dataset)

    os.makedirs(os.path.dirname(cfg.output) if os.path.dirname(cfg.output) else ".", exist_ok=True)
    dataset.to_csv(cfg.output, index=False)

    print(f"\nSaved: {cfg.output}")
    print(f"Rows : {len(dataset)} weekly observations")
    print(f"Cols : {list(dataset.columns)}")
    print("\nSample (first 3 rows):")
    print(dataset.head(3).to_string(index=False))
    print("\nSample (last 3 rows):")
    print(dataset.tail(3).to_string(index=False))

    # Summary of macro coverage
    print("\nMacro series coverage:")
    macro_cols = [col for _, col, _ in FRED_SERIES]
    for col in macro_cols:
        first_valid = dataset[col].first_valid_index()
        n_valid = dataset[col].notna().sum()
        print(f"  {col:20s}  {n_valid:4d}/{len(dataset)} weeks  first: {first_valid}")


if __name__ == "__main__":
    main()