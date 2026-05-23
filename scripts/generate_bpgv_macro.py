"""
Generate BPGV Macro Regime Data CSV for trade-ngin backtest.

Fetches building permit data (ALFRED vintage), computes the original BPGV
pipeline (PERMIT total, rolling-stdev volatility), AND the new paper-faithful
GARCH(1,1) pipeline (PERMIT1 single-family, expanding-window QMLE conditional
volatility). Writes a unified CSV consumed by the C++ BPGVRotationStrategy.

The schema preserves all existing columns at fixed positions for C++ backward
compatibility (macro_csv_loader.cpp parses positionally). New columns are
appended at the end; the existing C++ loader ignores them. To use the new
σ^GARCH signal in C++ requires extending MonthlyMacroRecord — see Step 3 of
the T1.1+T1.3 sequencing in reports/a1_data_sufficiency_for_garch.md.

CSV columns (in order):
  year, month, bpgv, bpgv_ewma, bpgv_percentile, yield_curve_spread,
  ewma_slope, regime_score, permit_growth, strong_risk_on,
  bpg_sfh, bpgv_sfh, bpgv_garch, bpgv_garch_percentile,
  garch_converged, garch_alpha, garch_beta

Where the appended columns are:
  bpg_sfh                — single-family permit log-return (paper's BPG)
  bpgv_sfh               — rolling 12m stdev of bpg_sfh (apples-to-apples
                            replacement for `bpgv` which uses PERMIT total)
  bpgv_garch             — GARCH(1,1) one-step-ahead conditional vol σ_{t+1|t}
                            on bpg_sfh; expanding-window QMLE, no look-ahead
  bpgv_garch_percentile  — rolling 60m percentile rank of bpgv_garch
  garch_converged        — 1 if QMLE converged that month, else 0
  garch_alpha, beta      — persistence parameters from that month's fit
                            (sanity check: α+β should be 0.95–0.99 per
                            Cortes & LaPoint 2025 §4)

Data sufficiency (Hwang & Valls Pereira 2006: GARCH(1,1) needs ≥500 monthly
obs for stable QMLE):
  FRED PERMIT1 starts 1960-01 → 796 obs at 2026-04 ✓
  At backtest start (2011-05) we have ~620 obs ✓
  See reports/a1_data_sufficiency_for_garch.md for the full rationale.

Usage:
    python scripts/generate_bpgv_macro.py [--output data/macro/bpgv_regime.csv]
                                          [--garch-min-history 240]
                                          [--no-garch]
"""

import argparse
import os
import sys
import time
import warnings

import numpy as np
import pandas as pd
import requests

# arch is required for the GARCH pipeline. If the user only wants the legacy
# rolling-stdev BPGV (--no-garch), the import is skipped.
try:
    from arch import arch_model
    ARCH_AVAILABLE = True
except ImportError:
    ARCH_AVAILABLE = False

FRED_API_KEY = "7a12d42437543703fc1ed16feb871ff2"
FRED_API_BASE = "https://api.stlouisfed.org/fred"

# BPGV parameters (must match C++ strategy defaults)
BPGV_WINDOW = 12
EWMA_SPAN = 6
PERCENTILE_WINDOW = 60

# Regime scoring thresholds
RISK_ON_BPGV_THRESHOLD = 75
RISK_ON_YC_THRESHOLD = -0.75

# GARCH parameters
DEFAULT_GARCH_MIN_HISTORY = 240  # ~20 years before first emission
GARCH_PERCENTILE_WINDOW = 60     # months for percentile rank


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


def get_alfred_permit_data(series_id, start_date, history_start_date=None):
    """Fetch a FRED building-permit series from ALFRED using first-release vintages.

    Args
    ----
    series_id : str
        FRED series ID (e.g. "PERMIT" or "PERMIT1").
    start_date : str
        Earliest observation date to include in the OUTPUT (ISO YYYY-MM-DD).
        For dates >= this, we hunt for the first-release vintage to enforce
        the no-look-ahead constraint.
    history_start_date : str | None
        If provided and earlier than start_date, also fetch observations from
        history_start_date..start_date but using the CURRENT vintage value
        (revisions to data older than 25-30 years don't affect any reasonable
        trader's information set at backtest start). This is the cheap path
        for GARCH warm-up history.

    Returns
    -------
    pd.DataFrame indexed by observation date with column `first_release_value`.
    """
    print(f"Fetching {series_id} data from ALFRED (start_date={start_date})...")

    # 1) Get the full vintage_date list for this series.
    url = f"{FRED_API_BASE}/series/vintagedates"
    params = {"series_id": series_id, "api_key": FRED_API_KEY, "file_type": "json"}
    response = request_with_retry(url, params)
    vintage_dates = response.json().get("vintage_dates", [])
    print(f"  {series_id}: {len(vintage_dates)} vintages available")

    # 2) Get current vintage observations from the earliest of {history_start, start}.
    fetch_start = history_start_date if history_start_date else start_date
    url = f"{FRED_API_BASE}/series/observations"
    params = {
        "series_id": series_id,
        "api_key": FRED_API_KEY,
        "file_type": "json",
        "vintage_dates": vintage_dates[-1],
        "observation_start": fetch_start,
        "limit": 100000,
    }
    response = request_with_retry(url, params)
    observations = response.json().get("observations", [])
    print(f"  {series_id}: {len(observations)} observations from {fetch_start}")

    # 3) For each observation, find the first-release vintage (only for dates
    #    >= start_date; for older history we use the current value).
    first_release_data = []
    cutoff = pd.to_datetime(start_date) if history_start_date else None

    for i, obs in enumerate(observations):
        if obs["value"] == ".":
            continue
        obs_date = pd.to_datetime(obs["date"])

        # Fast path for pre-start_date warm-up: use current value.
        if cutoff is not None and obs_date < cutoff:
            first_release_data.append({
                "date": obs_date,
                "first_release_value": float(obs["value"]),
                "first_release_vintage": pd.to_datetime(vintage_dates[-1]),
            })
            continue

        if (i + 1) % 50 == 0:
            print(f"  {series_id} vintage hunt: {i + 1}/{len(observations)}")

        # No-look-ahead path: hunt for first vintage published after obs.
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
            first_release_data.append({
                "date": obs_date,
                "first_release_value": first_release_value,
                "first_release_vintage": pd.to_datetime(first_release_vintage),
            })

    df = pd.DataFrame(first_release_data).set_index("date").sort_index()
    print(f"  {series_id}: assembled {len(df)} observations")
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
            data.append({"date": pd.to_datetime(obs["date"]),
                         "yield_curve": float(obs["value"])})

    df = pd.DataFrame(data).set_index("date")
    df_monthly = df.resample("ME").last()
    print(f"  Fetched {len(df_monthly)} monthly observations")
    return df_monthly


# --------------------------------------------------------------------------
# Legacy BPGV pipeline (PERMIT total, rolling stdev) — UNCHANGED for back-compat
# --------------------------------------------------------------------------
def calculate_bpgv(permit_df):
    """Calculate the original Building Permit Growth Volatility (rolling stdev)."""
    print("Calculating BPGV (PERMIT, rolling stdev)...")

    df = permit_df.copy()
    df["bpg"] = df["first_release_value"].pct_change()
    df["bpgv"] = df["bpg"].rolling(window=BPGV_WINDOW, min_periods=6).std()
    df["bpgv_ewma"] = df["bpgv"].ewm(span=EWMA_SPAN, adjust=False).mean()
    df["bpgv_ewma_slope"] = df["bpgv_ewma"].diff()
    df.index = df.index.to_period("M").to_timestamp("M")

    print(f"  BPGV mean: {df['bpgv'].mean() * 100:.2f}%")
    return df


# --------------------------------------------------------------------------
# New GARCH(1,1) pipeline (PERMIT1 single-family, expanding-window QMLE)
# --------------------------------------------------------------------------
def calculate_bpg_sfh(permit1_df):
    """Compute single-family permit log-return — paper's BPG.

    Paper §3.4 / eq (3.3): they use log-differences. pct_change is
    asymptotically equivalent for small changes but log-diff handles outliers
    (e.g., a +50% MoM jump translates to ~0.405 log-diff, not 0.50).
    """
    df = permit1_df.copy()
    # log(x_t / x_{t-1}) = log(x_t) - log(x_{t-1})
    df["bpg_sfh"] = np.log(df["first_release_value"]).diff()
    df["bpgv_sfh"] = df["bpg_sfh"].rolling(window=BPGV_WINDOW,
                                             min_periods=6).std()
    df.index = df.index.to_period("M").to_timestamp("M")
    return df


def fit_garch_one_step(g_series, dist="normal"):
    """Fit GARCH(1,1) via QMLE and return one-step-ahead σ + diagnostics.

    Returns
    -------
    dict with keys: sigma (float, NaN if fit failed),
                    converged (bool),
                    alpha (float, NaN if fit failed),
                    beta (float, NaN if fit failed).
    """
    # The arch library wants returns scaled to roughly unit variance for
    # numerical stability. Permit growth is on the order of ±10%, so multiplying
    # by 100 puts us in a comfortable regime; we divide σ back at the end.
    scaled = g_series.dropna().values * 100.0
    if len(scaled) < 100:
        return {"sigma": np.nan, "converged": False, "alpha": np.nan, "beta": np.nan}

    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            am = arch_model(scaled, vol="GARCH", p=1, q=1,
                            mean="Constant", dist=dist, rescale=False)
            res = am.fit(disp="off", show_warning=False)
    except Exception:  # pragma: no cover — convergence failure is expected occasionally
        return {"sigma": np.nan, "converged": False, "alpha": np.nan, "beta": np.nan}

    if not res.convergence_flag == 0:
        return {"sigma": np.nan, "converged": False, "alpha": np.nan, "beta": np.nan}

    # One-step-ahead variance forecast.
    fc = res.forecast(horizon=1, reindex=False)
    var_t1 = fc.variance.iloc[-1, 0]
    sigma_t1 = float(np.sqrt(max(var_t1, 0.0))) / 100.0  # un-scale

    # Parameter sanity: alpha (ARCH), beta (GARCH).
    alpha = float(res.params.get("alpha[1]", np.nan))
    beta = float(res.params.get("beta[1]", np.nan))

    return {"sigma": sigma_t1, "converged": True, "alpha": alpha, "beta": beta}


def calculate_bpgv_garch(bpg_sfh_df, min_history_obs=DEFAULT_GARCH_MIN_HISTORY):
    """Expanding-window GARCH(1,1) on bpg_sfh.

    For each month t with at least min_history_obs prior observations, fit
    GARCH(1,1) on bpg_sfh[:t] (data known by end of month t) and emit
    σ_{t+1|t} as bpgv_garch[t].

    Returns a DataFrame with the same index as bpg_sfh_df plus columns:
        bpgv_garch, garch_converged (0/1), garch_alpha, garch_beta.
    """
    if not ARCH_AVAILABLE:
        raise RuntimeError(
            "arch package not installed. Run `pip install arch` or use --no-garch."
        )

    print(f"Computing GARCH(1,1) σ_{{t+1|t}} forecasts "
          f"(expanding window, min_history={min_history_obs}m)...")

    g = bpg_sfh_df["bpg_sfh"].dropna()
    out = pd.DataFrame(index=bpg_sfh_df.index, columns=[
        "bpgv_garch", "garch_converged", "garch_alpha", "garch_beta"
    ], dtype=float)

    converged_count = 0
    total_fits = 0
    first_emission = None

    # For each month t (in g's index), fit on g[:t] and forecast σ_{t+1}.
    # We emit the forecast as bpgv_garch[t+1] so the row at month t+1 has the
    # σ that was knowable at end of month t — consistent with how the rest of
    # the macro CSV is structured (the C++ strategy reads month_t row at
    # rebalance time in month t+1).
    g_index = g.index
    for i in range(len(g_index)):
        if i + 1 < min_history_obs:
            continue
        window = g.iloc[: i + 1]
        if i % 50 == 0 or i + 1 == len(g_index):
            print(f"  GARCH fit {i + 1}/{len(g_index)}  ({g_index[i].date()})")

        result = fit_garch_one_step(window)
        total_fits += 1
        if result["converged"]:
            converged_count += 1

        # The σ_{t+1|t} forecast lands in next month's row.
        if i + 1 < len(g_index):
            forecast_date = g_index[i + 1]
            if first_emission is None:
                first_emission = forecast_date
            out.loc[forecast_date, "bpgv_garch"] = result["sigma"]
            out.loc[forecast_date, "garch_converged"] = 1.0 if result["converged"] else 0.0
            out.loc[forecast_date, "garch_alpha"] = result["alpha"]
            out.loc[forecast_date, "garch_beta"] = result["beta"]

    conv_rate = converged_count / max(total_fits, 1) * 100
    print(f"  GARCH convergence rate: {conv_rate:.1f}% ({converged_count}/{total_fits})")
    if first_emission is not None:
        print(f"  First emission: {first_emission.date()}, "
              f"last: {out['bpgv_garch'].last_valid_index().date()}")

    # Persistence sanity check on converged fits.
    persist = (out["garch_alpha"] + out["garch_beta"]).dropna()
    if len(persist) > 0:
        print(f"  α+β persistence: mean={persist.mean():.3f} "
              f"(paper expects 0.95–0.99); min={persist.min():.3f}, "
              f"max={persist.max():.3f}")

    return out


def add_garch_percentile(merged_df):
    """Rolling 60-month percentile rank of bpgv_garch."""
    merged_df["bpgv_garch_percentile"] = (
        merged_df["bpgv_garch"]
        .rolling(window=GARCH_PERCENTILE_WINDOW, min_periods=12)
        .apply(lambda x: pd.Series(x).rank(pct=True).iloc[-1] * 100,
               raw=False)
    )
    return merged_df


# --------------------------------------------------------------------------
# Regime classification (UNCHANGED — uses original bpgv columns)
# --------------------------------------------------------------------------
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


# --------------------------------------------------------------------------
# CSV writer — preserves legacy column order, appends new GARCH columns
# --------------------------------------------------------------------------
def write_csv(df, output_path, output_start_date=None):
    """Write the regime data CSV.

    The first 10 columns match the existing C++ MacroCSVLoader format
    exactly (year, month, bpgv, bpgv_ewma, bpgv_percentile, yield_curve_spread,
    ewma_slope, regime_score, permit_growth, strong_risk_on). The remaining
    columns (bpg_sfh, bpgv_sfh, bpgv_garch, bpgv_garch_percentile,
    garch_converged, garch_alpha, garch_beta) are appended at the end and
    are silently ignored by the existing C++ loader. Extending the loader
    enables reading the σ^GARCH signal in the strategy (step 3 of T1.1).
    """
    print(f"Writing CSV to {output_path}...")

    records = []
    for idx in df.index:
        if pd.isna(df.loc[idx, "bpgv_percentile"]):
            continue
        if output_start_date is not None and idx < pd.to_datetime(output_start_date):
            continue

        records.append({
            "year": idx.year,
            "month": idx.month,
            "bpgv": df.loc[idx, "bpgv"],
            "bpgv_ewma": df.loc[idx, "bpgv_ewma"],
            "bpgv_percentile": df.loc[idx, "bpgv_percentile"],
            "yield_curve_spread": (df.loc[idx, "yield_curve"]
                                    if not pd.isna(df.loc[idx, "yield_curve"]) else 0.0),
            "ewma_slope": (df.loc[idx, "bpgv_ewma_slope"]
                            if not pd.isna(df.loc[idx, "bpgv_ewma_slope"]) else 0.0),
            "regime_score": df.loc[idx, "regime_score"],
            "permit_growth": (df.loc[idx, "bpg"]
                               if not pd.isna(df.loc[idx, "bpg"]) else 0.0),
            "strong_risk_on": int(df.loc[idx, "strong_risk_on"]),
            # ---- new GARCH-pipeline columns (appended; ignored by legacy C++) ----
            "bpg_sfh": _safe_get(df, idx, "bpg_sfh"),
            "bpgv_sfh": _safe_get(df, idx, "bpgv_sfh"),
            "bpgv_garch": _safe_get(df, idx, "bpgv_garch"),
            "bpgv_garch_percentile": _safe_get(df, idx, "bpgv_garch_percentile"),
            "garch_converged": _safe_get(df, idx, "garch_converged", default=0.0),
            "garch_alpha": _safe_get(df, idx, "garch_alpha"),
            "garch_beta": _safe_get(df, idx, "garch_beta"),
        })

    out_df = pd.DataFrame(records)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    out_df.to_csv(output_path, index=False, float_format="%.6f")
    print(f"  Wrote {len(out_df)} records ({out_df['year'].min()}-{out_df['year'].max()})")
    print(f"  Schema: {list(out_df.columns)}")


def _safe_get(df, idx, col, default=0.0):
    """Look up df[idx, col]; return default if missing or NaN."""
    if col not in df.columns:
        return default
    val = df.loc[idx, col]
    if pd.isna(val):
        return default
    return val


# --------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Generate BPGV macro regime CSV")
    parser.add_argument("--output", default="data/macro/bpgv_regime.csv")
    parser.add_argument("--start-date", default="2000-01-01",
                        help="Earliest first-release-vintage observation "
                             "(no-look-ahead horizon, default 2000-01-01)")
    parser.add_argument("--garch-history-start", default="1960-01-01",
                        help="Earliest historical observation used to seed "
                             "GARCH (pre-start_date uses current vintage, "
                             "default 1960-01-01)")
    parser.add_argument("--garch-min-history", type=int,
                        default=DEFAULT_GARCH_MIN_HISTORY,
                        help=f"Minimum history months before GARCH emission "
                             f"(default {DEFAULT_GARCH_MIN_HISTORY})")
    parser.add_argument("--no-garch", action="store_true",
                        help="Skip GARCH pipeline (legacy CSV only)")
    parser.add_argument("--output-start-date", default=None,
                        help="If set, drop CSV rows before this date "
                             "(useful for trimming warm-up after GARCH)")
    args = parser.parse_args()

    if not args.no_garch and not ARCH_AVAILABLE:
        print("ERROR: arch package not installed; run `pip install arch` "
              "or use --no-garch.", file=sys.stderr)
        sys.exit(1)

    print("=" * 60)
    print("BPGV Macro Regime Data Generator")
    print("=" * 60)

    # 1) Legacy pipeline — PERMIT (all units), rolling-stdev BPGV.
    permit_df = get_alfred_permit_data(
        series_id="PERMIT", start_date=args.start_date)
    yc_df = get_yield_curve_data(start_date=args.start_date)
    bpgv_df = calculate_bpgv(permit_df)
    regime_df = classify_regimes(bpgv_df, yc_df)

    # 2) New pipeline — PERMIT1 (single-family), GARCH(1,1).
    if args.no_garch:
        print("--no-garch: skipping PERMIT1 + GARCH pipeline.")
        combined = regime_df
    else:
        permit1_df = get_alfred_permit_data(
            series_id="PERMIT1",
            start_date=args.start_date,
            history_start_date=args.garch_history_start)
        bpg_sfh_df = calculate_bpg_sfh(permit1_df)
        garch_df = calculate_bpgv_garch(
            bpg_sfh_df, min_history_obs=args.garch_min_history)
        # Merge bpg_sfh + bpgv_sfh + GARCH columns into regime_df.
        garch_combined = bpg_sfh_df[["bpg_sfh", "bpgv_sfh"]].join(garch_df, how="outer")
        combined = regime_df.join(garch_combined, how="left")
        combined = add_garch_percentile(combined)

    write_csv(combined, args.output,
              output_start_date=args.output_start_date)

    print("=" * 60)
    print("Done.")


if __name__ == "__main__":
    main()
