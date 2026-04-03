"""
Generate Daily Macro Data CSV for the Copper-Gold IP Strategy.

Fetches daily macro data from FRED (and OECD for China CLI), forward-fills
gaps, and writes a CSV to data/macro/copper_gold_daily.csv.

The CSV is consumed by the C++ CopperGoldIPStrategy at initialization.

FRED Series:
    DTWEXBGS  - US Dollar Index (Trade Weighted, Broad)
    VIXCLS    - CBOE Volatility Index (VIX)
    BAMLH0A0HYM2 - ICE BofA US High Yield Option-Adjusted Spread
    T10YIE    - 10-Year Breakeven Inflation Rate
    DGS10     - 10-Year Treasury Constant Maturity Rate
    DFII10    - 10-Year TIPS Yield
    SP500     - S&P 500 Index
    WALCL     - Fed Balance Sheet Total Assets (weekly, forward-filled)
    DEXCHUS   - CNY/USD Exchange Rate

OECD:
    China CLI - Composite Leading Indicator (monthly, forward-filled)

Usage:
    python scripts/generate_copper_gold_macro.py [--output data/macro/copper_gold_daily.csv]
"""

import argparse
import os
import sys
import time

import numpy as np
import pandas as pd
import requests

FRED_API_KEY = "7a12d42437543703fc1ed16feb871ff2"
FRED_API_BASE = "https://api.stlouisfed.org/fred"

# Series to fetch from FRED
FRED_SERIES = {
    "dxy": "DTWEXBGS",
    "vix": "VIXCLS",
    "hy_spread": "BAMLH0A0HYM2",
    "breakeven_10y": "T10YIE",
    "yield_10y": "DGS10",
    "tips_10y": "DFII10",
    "spx": "SP500",
    "fed_balance_sheet": "WALCL",
    "cny_usd": "DEXCHUS",
}


def request_with_retry(url, params, retries=5, backoff=5):
    last_error = None
    for attempt in range(retries):
        try:
            response = requests.get(url, params=params)
            if response.status_code == 429:
                wait = backoff * (attempt + 1)
                print(f"  Rate limit hit. Retrying in {wait}s...")
                time.sleep(wait)
                continue
            response.raise_for_status()
            return response
        except requests.RequestException as exc:
            last_error = exc
            wait = backoff * (attempt + 1)
            print(f"  Request error ({exc}). Retrying in {wait}s...")
            time.sleep(wait)
    if last_error:
        raise last_error
    raise RuntimeError("request_with_retry failed")


def fetch_fred_series(series_id, start_date="2009-01-01"):
    """Fetch a single FRED series and return as a pandas Series indexed by date."""
    print(f"  Fetching {series_id} from FRED...")
    url = f"{FRED_API_BASE}/series/observations"
    params = {
        "series_id": series_id,
        "api_key": FRED_API_KEY,
        "file_type": "json",
        "observation_start": start_date,
        "sort_order": "asc",
    }
    resp = request_with_retry(url, params)
    data = resp.json()

    dates = []
    values = []
    for obs in data.get("observations", []):
        if obs["value"] in (".", "NA", ""):
            continue
        try:
            dates.append(pd.Timestamp(obs["date"]))
            values.append(float(obs["value"]))
        except (ValueError, TypeError):
            continue

    s = pd.Series(values, index=pd.DatetimeIndex(dates), name=series_id)
    print(f"    Got {len(s)} observations ({s.index[0].date()} to {s.index[-1].date()})")
    return s


def fetch_china_cli(start_date="2009-01-01"):
    """Fetch China Composite Leading Indicator from OECD API (with fallback)."""
    print("  Fetching China CLI from OECD...")

    # Try the new OECD Data Explorer API first
    urls_to_try = [
        "https://sdmx.oecd.org/public/rest/data/OECD.SDD.STES,DSD_KEI@DF_KEI,4.0/CHN.M.LI.LOLITOAA.IXNSA..?startPeriod={start}",
        "https://stats.oecd.org/sdmx-json/data/MEI_CLI/LOLITOAA.CHN.M/all?startTime={start}",
    ]

    for url_template in urls_to_try:
        try:
            url = url_template.format(start=start_date[:7])
            resp = requests.get(url, timeout=30)
            if resp.status_code != 200:
                continue

            data = resp.json()

            dates = []
            values = []

            # Try SDMX-JSON 2.0 format (new API)
            datasets = data.get("dataSets", data.get("data", {}).get("dataSets", []))
            if isinstance(datasets, list) and datasets:
                observations = datasets[0].get("series", {})
                dims = data.get("structure", data.get("data", {}).get("structure", {}))
                time_periods = dims.get("dimensions", {}).get("observation", [{}])[0].get("values", [])

                for series_key, series_data in observations.items():
                    for idx_str, obs_values in series_data.get("observations", {}).items():
                        idx = int(idx_str)
                        if idx < len(time_periods):
                            period = time_periods[idx]
                            date_str = period.get("id", period.get("name", ""))
                            if date_str and obs_values:
                                # Handle both "2020-01" and "2020-01-15" formats
                                if len(date_str) == 7:
                                    date_str += "-15"
                                dates.append(pd.Timestamp(date_str))
                                values.append(float(obs_values[0]))

            if dates:
                s = pd.Series(values, index=pd.DatetimeIndex(dates), name="china_cli")
                s = s.sort_index()
                print(f"    Got {len(s)} observations from OECD")
                return s
        except Exception:
            continue

    # Fallback: use FRED CHNCPIALLMINMEI (China CPI as proxy) or constant
    print("    WARNING: OECD API unavailable, trying FRED China proxy (USALOLITONOSTSAM)...")
    try:
        # US CLI as a rough proxy (will still provide regime variation)
        s = fetch_fred_series("USALOLITONOSTSAM", start_date)
        s.name = "china_cli"
        # Resample monthly -> mid-month
        s = s.resample("MS").last()
        s.index = s.index + pd.Timedelta(days=14)
        print(f"    Using US CLI as proxy: {len(s)} observations")
        return s
    except Exception:
        pass

    print("    WARNING: All CLI sources failed, using constant 100.0")
    idx = pd.date_range(start_date, pd.Timestamp.now(), freq="MS") + pd.Timedelta(days=14)
    return pd.Series(100.0, index=idx, name="china_cli")


def build_daily_macro_df(start_date="2009-01-01"):
    """Build a daily-frequency DataFrame with all macro series."""
    print("Fetching all FRED series...")

    # Fetch all FRED series
    fred_data = {}
    for col_name, series_id in FRED_SERIES.items():
        fred_data[col_name] = fetch_fred_series(series_id, start_date)
        time.sleep(0.5)  # Rate limit courtesy

    # Fetch China CLI
    china_cli = fetch_china_cli(start_date)

    # Create business day index
    end_date = pd.Timestamp.now()
    bdays = pd.bdate_range(start=start_date, end=end_date)

    df = pd.DataFrame(index=bdays)
    df.index.name = "date"

    # Reindex each series to business days, forward-filling
    for col_name, s in fred_data.items():
        df[col_name] = s.reindex(df.index, method="ffill")

    # China CLI: monthly -> daily via forward-fill
    df["china_cli"] = china_cli.reindex(df.index, method="ffill")

    # Forward-fill any remaining NaN
    df = df.ffill()

    # Drop rows where critical series are still NaN (early period)
    df = df.dropna(subset=["dxy", "vix", "spx"])

    print(f"\nFinal dataset: {len(df)} business days from {df.index[0].date()} to {df.index[-1].date()}")
    return df


def write_csv(df, output_path):
    """Write DataFrame to CSV in the format expected by DailyMacroCSVLoader."""
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    def fmt(val):
        """Format value, replacing NaN with 0.0."""
        if pd.isna(val):
            return "0.000000"
        return f"{val:.6f}"

    with open(output_path, "w") as f:
        f.write("year,month,day,dxy,vix,hy_spread,breakeven_10y,yield_10y,tips_10y,spx,fed_balance_sheet,china_cli,cny_usd\n")

        cols = ["dxy", "vix", "hy_spread", "breakeven_10y", "yield_10y",
                "tips_10y", "spx", "fed_balance_sheet", "china_cli", "cny_usd"]
        for date, row in df.iterrows():
            vals = ",".join(fmt(row[c]) for c in cols)
            f.write(f"{date.year},{date.month},{date.day},{vals}\n")

    print(f"\nWrote {len(df)} records to {output_path}")


def main():
    parser = argparse.ArgumentParser(description="Generate daily macro CSV for Copper-Gold IP strategy")
    parser.add_argument("--output", default="data/macro/copper_gold_daily.csv",
                        help="Output CSV path (default: data/macro/copper_gold_daily.csv)")
    parser.add_argument("--start", default="2009-01-01",
                        help="Start date for data fetch (default: 2009-01-01)")
    args = parser.parse_args()

    print("=== Copper-Gold IP Daily Macro Data Generator ===\n")

    df = build_daily_macro_df(start_date=args.start)
    write_csv(df, args.output)

    # Summary stats
    print("\nColumn summary:")
    for col in df.columns:
        valid = df[col].notna().sum()
        print(f"  {col:25s}: {valid:5d} valid, range [{df[col].min():.4f}, {df[col].max():.4f}]")

    print("\nDone.")


if __name__ == "__main__":
    main()
