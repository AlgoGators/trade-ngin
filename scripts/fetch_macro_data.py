#!/usr/bin/env python3
"""
Fetch all macro series for the DFM from FRED API and save as CSVs.

Usage:
    pip install fredapi pandas
    export FRED_API_KEY="your_key_here"   # get one free at https://fred.stlouisfed.org/docs/api/api_key.html
    python fetch_macro_data.py <output_dir>

Output: one CSV per series in <output_dir>/, format: date,value
"""

import os
import sys
import pandas as pd
from fredapi import Fred

# FRED series IDs mapped to our CSV filenames
SERIES = {
    # Inflation
    "cpi":                   "CPIAUCSL",        # CPI All Urban Consumers
    "core_cpi":              "CPILFESL",        # CPI Less Food and Energy
    "core_pce":              "PCEPILFE",        # PCE Less Food and Energy
    "breakeven_5y":          "T5YIE",           # 5-Year Breakeven Inflation Rate

    # Growth / Jobs
    "nonfarm_payrolls":      "PAYEMS",          # Total Nonfarm Payrolls
    "unemployment_rate":     "UNRATE",          # Unemployment Rate
    "ism_manufacturing_pmi": "MANEMP",          # ISM Manufacturing PMI (proxy: mfg employment)
    "industrial_production": "INDPRO",          # Industrial Production Index
    "retail_sales":          "RSAFS",           # Advance Retail Sales
    "gdp":                   "GDP",             # Gross Domestic Product

    # Yield Curve
    "treasury_2y":           "DGS2",            # 2-Year Treasury Constant Maturity Rate
    "treasury_10y":          "DGS10",           # 10-Year Treasury Constant Maturity Rate
    "yield_spread_10y_2y":   "T10Y2Y",          # 10-Year Treasury Minus 2-Year
    "fed_funds_rate":        "FEDFUNDS",        # Effective Federal Funds Rate
    "ig_credit_spread":      "BAMLC0A4CBBB",    # ICE BofA BBB US Corp Index OAS
    "high_yield_spread":     "BAMLH0A0HYM2",    # ICE BofA US High Yield Index OAS

    # Liquidity
    "m2_money_supply":       "M2SL",            # M2 Money Stock
    "ted_spread":            "TEDRATE",         # TED Spread (discontinued 2022, partial)
    "fed_balance_sheet":     "WALCL",           # Fed Total Assets

    # Market
    "vix":                   "VIXCLS",          # CBOE VIX
    "dxy":                   "DTWEXBGS",        # Trade Weighted US Dollar Index
    "tips_10y":              "DFII10",          # 10-Year TIPS
    "gdp_nowcast":           "GDPNOW",         # Atlanta Fed GDPNow (may not be on FRED)
}


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <output_dir>")
        sys.exit(1)

    output_dir = sys.argv[1]
    os.makedirs(output_dir, exist_ok=True)

    api_key = os.environ.get("FRED_API_KEY")
    if not api_key:
        print("ERROR: Set FRED_API_KEY environment variable")
        print("  Get a free key at: https://fred.stlouisfed.org/docs/api/api_key.html")
        sys.exit(1)

    fred = Fred(api_key=api_key)

    success = 0
    failed = []

    for csv_name, fred_id in SERIES.items():
        try:
            print(f"Fetching {csv_name} ({fred_id})...", end=" ", flush=True)
            series = fred.get_series(fred_id)
            df = series.dropna().reset_index()
            df.columns = ["date", "value"]
            df["date"] = pd.to_datetime(df["date"]).dt.strftime("%Y-%m-%d")

            path = os.path.join(output_dir, f"{csv_name}.csv")
            df.to_csv(path, index=False)
            print(f"OK ({len(df)} records)")
            success += 1
        except Exception as e:
            print(f"FAILED: {e}")
            failed.append(csv_name)

    print(f"\nDone: {success}/{len(SERIES)} series fetched")
    if failed:
        print(f"Failed: {', '.join(failed)}")
        print("(Some series like gdp_nowcast may not be available on FRED)")


if __name__ == "__main__":
    main()
