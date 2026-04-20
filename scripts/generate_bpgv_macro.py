"""
Generate BPGV Macro Regime Data CSV for trade-ngin backtest.

Fetches building permit data (ALFRED vintage) and yield curve data (FRED),
computes the full BPGV pipeline (growth rates, volatility, EWMA, percentiles,
regime scores), and writes a CSV to data/macro/bpgv_regime.csv.

The CSV is consumed by the C++ BPGVRotationStrategy at initialization.

Usage:
    python scripts/generate_bpgv_macro.py [--output data/macro/bpgv_regime.csv]
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

# BPGV parameters (must match C++ strategy defaults)
BPGV_WINDOW = 12
EWMA_SPAN = 6
PERCENTILE_WINDOW = 60

# Regime scoring thresholds
RISK_ON_BPGV_THRESHOLD = 75
RISK_ON_YC_THRESHOLD = -0.75


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


def get_alfred_permit_data(start_date="2000-01-01"):
    """Fetch PERMIT data from ALFRED using first-release vintages."""
    print("Fetching building permit data from ALFRED...")

    series_id = "PERMIT"

    # Get vintage dates
    url = f"{FRED_API_BASE}/series/vintagedates"
    params = {"series_id": series_id, "api_key": FRED_API_KEY, "file_type": "json"}
    response = request_with_retry(url, params)
    vintage_dates = response.json().get("vintage_dates", [])
    print(f"  Found {len(vintage_dates)} vintages")

    # Get current vintage observations
    url = f"{FRED_API_BASE}/series/observations"
    params = {
        "series_id": series_id,
        "api_key": FRED_API_KEY,
        "file_type": "json",
        "vintage_dates": vintage_dates[-1],
        "observation_start": start_date,
        "limit": 100000,
    }
    response = request_with_retry(url, params)
    observations = response.json().get("observations", [])
    print(f"  Found {len(observations)} observations")

    # Build first-release dataset
    first_release_data = []
    print("  Fetching first-release vintages...")

    for i, obs in enumerate(observations):
        if (i + 1) % 20 == 0:
            print(f"  Progress: {i + 1}/{len(observations)}")

        obs_date = pd.to_datetime(obs["date"])
        estimated_release = obs_date + pd.DateOffset(months=1, days=18)
        relevant_vintages = [
            v for v in vintage_dates if pd.to_datetime(v) >= estimated_release
        ]

        if not relevant_vintages:
            continue

        first_release_value = None
        first_release_vintage = None

        for vintage_date in relevant_vintages[:10]:
            url = f"{FRED_API_BASE}/series/observations"
            params = {
                "series_id": series_id,
                "api_key": FRED_API_KEY,
                "file_type": "json",
                "vintage_dates": vintage_date,
                "observation_start": obs["date"],
                "observation_end": obs["date"],
            }

            try:
                resp = request_with_retry(url, params)
                vintage_obs = resp.json().get("observations", [])
                if vintage_obs and vintage_obs[0]["value"] != ".":
                    first_release_vintage = vintage_date
                    first_release_value = float(vintage_obs[0]["value"])
                    break
            except requests.RequestException:
                continue

        if first_release_value is not None:
            first_release_data.append(
                {
                    "date": obs_date,
                    "first_release_value": first_release_value,
                    "first_release_vintage": pd.to_datetime(first_release_vintage),
                }
            )

    df = pd.DataFrame(first_release_data).set_index("date").sort_index()
    print(f"  Fetched {len(df)} first-release observations")
    return df


def get_yield_curve_data(start_date="2000-01-01"):
    """Fetch 10Y-2Y Treasury spread from FRED."""
    print("Fetching yield curve data from FRED...")

    url = f"{FRED_API_BASE}/series/observations"
    params = {
        "series_id": "T10Y2Y",
        "api_key": FRED_API_KEY,
        "file_type": "json",
        "observation_start": start_date,
        "limit": 100000,
    }
    response = request_with_retry(url, params)
    observations = response.json().get("observations", [])

    data = []
    for obs in observations:
        if obs["value"] != ".":
            data.append(
                {
                    "date": pd.to_datetime(obs["date"]),
                    "yield_curve": float(obs["value"]),
                }
            )

    df = pd.DataFrame(data).set_index("date")
    df_monthly = df.resample("ME").last()
    print(f"  Fetched {len(df_monthly)} monthly observations")
    return df_monthly


def calculate_bpgv(permit_df):
    """Calculate Building Permit Growth Volatility."""
    print("Calculating BPGV...")

    df = permit_df.copy()
    df["bpg"] = df["first_release_value"].pct_change()
    df["bpgv"] = df["bpg"].rolling(window=BPGV_WINDOW, min_periods=6).std()
    df["bpgv_ewma"] = df["bpgv"].ewm(span=EWMA_SPAN, adjust=False).mean()
    df["bpgv_ewma_slope"] = df["bpgv_ewma"].diff()
    df.index = df.index.to_period("M").to_timestamp("M")

    print(f"  BPGV mean: {df['bpgv'].mean() * 100:.2f}%")
    return df


def classify_regimes(bpgv_df, yc_df):
    """Compute regime scores using BPGV, yield curve, and EWMA slope."""
    print("Classifying regimes...")

    df = bpgv_df.copy()
    df = df.join(yc_df, how="left")
    df["yield_curve"] = df["yield_curve"].ffill(limit=3)

    # Rolling percentile (backward-looking only)
    df["bpgv_percentile"] = (
        df["bpgv"]
        .rolling(window=PERCENTILE_WINDOW, min_periods=12)
        .apply(lambda x: pd.Series(x).rank(pct=True).iloc[-1] * 100)
    )

    # Composite regime score
    df["regime_score"] = 0.0

    for idx in df.index:
        bpgv_pct = df.loc[idx, "bpgv_percentile"]
        yc = df.loc[idx, "yield_curve"]
        ewma_slope = df.loc[idx, "bpgv_ewma_slope"]

        if pd.isna(bpgv_pct):
            continue

        score = 0.0
        score += (bpgv_pct - 50) / 100  # [-0.5, +0.5]

        if not pd.isna(yc):
            if yc < -0.5:
                score += 0.3
            elif yc < 0:
                score += 0.15
            elif yc > 1.5:
                score -= 0.15

        if not pd.isna(ewma_slope):
            if ewma_slope > 0:
                score += 0.2
            else:
                score -= 0.2

        df.loc[idx, "regime_score"] = np.clip(score, -1, 1)

    # Strong risk-on flag
    df["strong_risk_on"] = (
        (df["bpgv_percentile"] < RISK_ON_BPGV_THRESHOLD)
        & (df["yield_curve"] > RISK_ON_YC_THRESHOLD)
        & (df["bpgv_ewma_slope"] <= 0)
    )

    print(f"  Risk-on months: {(df['regime_score'] <= -0.05).sum()}")
    print(f"  Neutral months: {((df['regime_score'] > -0.05) & (df['regime_score'] < 0.20)).sum()}")
    print(f"  Risk-off months: {(df['regime_score'] >= 0.20).sum()}")
    print(f"  Strong risk-on: {df['strong_risk_on'].sum()}")

    return df


def write_csv(df, output_path):
    """Write the regime data CSV for C++ consumption."""
    print(f"Writing CSV to {output_path}...")

    records = []
    for idx in df.index:
        if pd.isna(df.loc[idx, "bpgv_percentile"]):
            continue

        records.append(
            {
                "year": idx.year,
                "month": idx.month,
                "bpgv": df.loc[idx, "bpgv"],
                "bpgv_ewma": df.loc[idx, "bpgv_ewma"],
                "bpgv_percentile": df.loc[idx, "bpgv_percentile"],
                "yield_curve_spread": df.loc[idx, "yield_curve"]
                if not pd.isna(df.loc[idx, "yield_curve"])
                else 0.0,
                "ewma_slope": df.loc[idx, "bpgv_ewma_slope"]
                if not pd.isna(df.loc[idx, "bpgv_ewma_slope"])
                else 0.0,
                "regime_score": df.loc[idx, "regime_score"],
                "permit_growth": df.loc[idx, "bpg"]
                if not pd.isna(df.loc[idx, "bpg"])
                else 0.0,
                "strong_risk_on": int(df.loc[idx, "strong_risk_on"]),
            }
        )

    out_df = pd.DataFrame(records)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    out_df.to_csv(output_path, index=False, float_format="%.6f")
    print(f"  Wrote {len(out_df)} records ({out_df['year'].min()}-{out_df['year'].max()})")


def main():
    parser = argparse.ArgumentParser(description="Generate BPGV macro regime CSV")
    parser.add_argument(
        "--output",
        default="data/macro/bpgv_regime.csv",
        help="Output CSV path (default: data/macro/bpgv_regime.csv)",
    )
    parser.add_argument(
        "--start-date",
        default="2000-01-01",
        help="Start date for data fetch (default: 2000-01-01)",
    )
    args = parser.parse_args()

    print("=" * 60)
    print("BPGV Macro Regime Data Generator")
    print("=" * 60)

    permit_df = get_alfred_permit_data(start_date=args.start_date)
    yc_df = get_yield_curve_data(start_date=args.start_date)
    bpgv_df = calculate_bpgv(permit_df)
    regime_df = classify_regimes(bpgv_df, yc_df)
    write_csv(regime_df, args.output)

    print("=" * 60)
    print("Done.")


if __name__ == "__main__":
    main()
