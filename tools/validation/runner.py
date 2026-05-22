"""
Validation harness top-level runner.

Reads a backtest equity curve from either a CSV or Postgres, runs the
full DSR / Harvey-Liu / Politis-Romano / Ledoit-Wolf pipeline against
three benchmarks (SPY, 60/40, equal-weight SPY/TLT/GLD), and emits a
Markdown report.

Usage:
    # CSV input — expects columns: date, equity (portfolio value).
    python -m tools.validation.runner --equity-csv path/to/equity_curve.csv

    # Postgres input — requires PG* env vars and backtest.equity_curve
    # table populated by a prior run.
    python -m tools.validation.runner --run-id BPGV_ROTATION_20260421_XXXXXX_YYY

Output:
    reports/validation_<run_id or stem>.md
"""

from __future__ import annotations

import argparse
import os
import sys
import textwrap
from datetime import datetime
from pathlib import Path

import numpy as np
import pandas as pd

# Add tools/ to path so relative imports work when invoked as script.
_THIS = Path(__file__).resolve()
sys.path.insert(0, str(_THIS.parents[2]))

from tools.validation import benchmarks, dsr, harvey_liu, politis_romano  # noqa: E402


REPO_ROOT = _THIS.parents[2]
REPORTS_DIR = REPO_ROOT / "reports"


def load_equity_from_csv(path: Path) -> pd.Series:
    df = pd.read_csv(path, parse_dates=["date"])
    df = df.set_index("date").sort_index()
    for col in ("equity", "portfolio_value", "value"):
        if col in df.columns:
            return df[col].astype(float)
    raise KeyError(f"No equity column in {path}; have {df.columns.tolist()}")


def load_equity_from_postgres(run_id: str) -> pd.Series:
    try:
        import psycopg2  # type: ignore
    except ImportError as e:
        raise RuntimeError("psycopg2 not available; install or use --equity-csv") from e

    conn = psycopg2.connect(
        host=os.environ.get("PGHOST", "localhost"),
        port=int(os.environ.get("PGPORT", 5432)),
        user=os.environ.get("PGUSER", "postgres"),
        password=os.environ.get("PGPASSWORD", ""),
        dbname=os.environ.get("PGDATABASE", "trade_ngin"),
    )
    sql = """
        SELECT timestamp AT TIME ZONE 'UTC' AS date, equity
        FROM backtest.equity_curve
        WHERE run_id = %s
        ORDER BY timestamp
    """
    df = pd.read_sql(sql, conn, params=(run_id,))
    conn.close()
    if df.empty:
        raise RuntimeError(f"No equity curve rows for run_id={run_id}")
    df["date"] = pd.to_datetime(df["date"])
    df = df.set_index("date").sort_index()
    return df["equity"].astype(float)


def compute_daily_returns(equity: pd.Series) -> pd.Series:
    return equity.pct_change().dropna()


def align_to_benchmarks(
    strategy_returns: pd.Series,
    bmk: pd.DataFrame,
) -> tuple[pd.Series, pd.DataFrame]:
    """Inner-join the strategy returns with the benchmark matrix."""
    common = strategy_returns.index.intersection(bmk.index)
    return strategy_returns.loc[common], bmk.loc[common]


def render_report(
    label: str,
    strategy_returns: pd.Series,
    bmk: pd.DataFrame,
    n_trials_choices: tuple[int, ...] = (10, 30, 100),
    n_boot: int = 10000,
    rng_seed: int = 42,
) -> str:
    lines: list[str] = []
    lines.append(f"# Validation Report — {label}")
    lines.append("")
    lines.append(f"Generated: {datetime.utcnow().isoformat(timespec='seconds')} UTC")
    lines.append(f"Window: {strategy_returns.index.min().date()} .. "
                 f"{strategy_returns.index.max().date()}")
    lines.append(f"T = {strategy_returns.size} daily returns")
    lines.append("")

    # --- Raw metrics ------------------------------------------------------
    sr = dsr.sharpe_ratio(strategy_returns.values)
    lines.append("## Raw performance")
    lines.append(f"- Annualized Sharpe: **{sr:.3f}**")
    lines.append(f"- Annualized mean: {strategy_returns.mean() * 252:.4f}")
    lines.append(f"- Annualized vol: {strategy_returns.std(ddof=1) * np.sqrt(252):.4f}")
    lines.append("")

    # --- DSR at several N -------------------------------------------------
    lines.append("## Deflated Sharpe (Bailey-Lopez de Prado 2014)")
    lines.append("| N trials | SR hat | E[max SR null] | DSR | Reject null at α=5 %? |")
    lines.append("|---:|---:|---:|---:|---:|")
    for n in n_trials_choices:
        d, sh, null = dsr.deflated_sharpe(strategy_returns.values, n_trials=n)
        pass_fail = "✓" if (d == d and d >= 0.95) else "✗"
        lines.append(f"| {n} | {sh:.3f} | {null:.3f} | {d:.3f} | {pass_fail} |")
    lines.append("")

    # --- Harvey-Liu haircuts ----------------------------------------------
    lines.append("## Harvey-Liu-Zhu multiple-testing haircuts")
    lines.append("Assumes strategy is one of N trials; other N-1 treated as p=0.5 (HLZ framing).")
    lines.append("| N | Bonferroni p | Bonferroni SR | Holm p | BHY p | Reject@5% (Bonf / Holm / BHY) |")
    lines.append("|---:|---:|---:|---:|---:|---|")
    for n in n_trials_choices:
        rep = harvey_liu.haircut_report(sr, strategy_returns.size, n)
        bonf = "✓" if rep["reject_5pct_bonferroni"] else "✗"
        holm = "✓" if rep["reject_5pct_holm"] else "✗"
        bhy = "✓" if rep["reject_5pct_bhy"] else "✗"
        lines.append(
            f"| {n} | {rep['bonferroni_p']:.3f} | "
            f"{rep['bonferroni_sr']:.3f} | {rep['holm_p']:.3f} | "
            f"{rep['bhy_p']:.3f} | {bonf} / {holm} / {bhy} |"
        )
    lines.append("")

    # --- Politis-Romano bootstrap CI --------------------------------------
    lines.append("## Politis-Romano stationary block bootstrap Sharpe CI")
    ci = politis_romano.sharpe_bootstrap_ci(
        strategy_returns.values, n_boot=n_boot, rng_seed=rng_seed,
    )
    lines.append(f"- Block length (PW 2004 auto): {ci['block_length']:.1f} days")
    lines.append(f"- Bootstrap Sharpe mean: {ci['mean_sr']:.3f}")
    lines.append(f"- 95 % CI: [{ci['ci_low']:.3f}, {ci['ci_high']:.3f}]")
    lines.append(f"- Bootstrap draws: {ci['n_boot']}")
    lines.append("")

    # --- Ledoit-Wolf vs benchmarks ---------------------------------------
    lines.append("## Ledoit-Wolf pairwise Sharpe tests vs benchmarks")
    lines.append("One-sided H1: strategy SR > benchmark SR. p < 0.05 = strategy significantly beats benchmark.")
    lines.append("| Benchmark | SR strat | SR bmk | Diff | Two-sided p | One-sided reject |")
    lines.append("|---|---:|---:|---:|---:|---|")
    for col in ("spy", "sixty_forty", "ew_spy_tlt_gld"):
        lw = politis_romano.ledoit_wolf_sharpe_test(
            strategy_returns.values,
            bmk[col].values,
            n_boot=n_boot,
            rng_seed=rng_seed,
        )
        if "error" in lw:
            lines.append(f"| {col} | - | - | - | - | {lw['error']} |")
            continue
        rej = "✓" if lw["reject_5pct_one_sided"] else "✗"
        lines.append(
            f"| {col} | {lw['sr_strategy']:.3f} | {lw['sr_benchmark']:.3f} | "
            f"{lw['sr_diff']:+.3f} | {lw['p_value_two_sided']:.3f} | {rej} |"
        )
    lines.append("")

    # --- Summary / gate verdict ------------------------------------------
    lines.append("## Gate verdict")
    d_30, _, _ = dsr.deflated_sharpe(strategy_returns.values, n_trials=30)
    rep_30 = harvey_liu.haircut_report(sr, strategy_returns.size, 30)
    lw_ew = politis_romano.ledoit_wolf_sharpe_test(
        strategy_returns.values,
        bmk["ew_spy_tlt_gld"].values,
        n_boot=n_boot,
        rng_seed=rng_seed,
    )

    passes = []
    passes.append(("DSR (N=30) ≥ 0.95", d_30 >= 0.95))
    passes.append(("HL Bonferroni p < 0.05 (N=30)", rep_30["reject_5pct_bonferroni"]))
    if "error" not in lw_ew:
        passes.append(
            ("Beats EW SPY/TLT/GLD at LW one-sided 5 %",
             lw_ew["reject_5pct_one_sided"]),
        )

    for name, ok in passes:
        lines.append(f"- {'✓' if ok else '✗'} {name}")

    overall = all(ok for _, ok in passes)
    lines.append("")
    lines.append(f"**Overall: {'PASS' if overall else 'FAIL'}**")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--equity-csv", type=Path, help="CSV with columns date, equity")
    src.add_argument("--run-id", type=str, help="Postgres backtest run_id")
    parser.add_argument("--label", type=str, default=None,
                        help="Report label (defaults to csv stem or run_id)")
    parser.add_argument("--n-boot", type=int, default=10000,
                        help="Bootstrap draws (default 10000)")
    parser.add_argument("--rng-seed", type=int, default=42)
    parser.add_argument("--start", type=str, default=None)
    parser.add_argument("--end", type=str, default=None)
    parser.add_argument("--output", type=Path, default=None,
                        help="Output Markdown path (default reports/<label>.md)")
    args = parser.parse_args()

    if args.equity_csv:
        equity = load_equity_from_csv(args.equity_csv)
        label = args.label or args.equity_csv.stem
    else:
        equity = load_equity_from_postgres(args.run_id)
        label = args.label or args.run_id

    returns = compute_daily_returns(equity)
    if args.start:
        returns = returns[returns.index >= pd.Timestamp(args.start)]
    if args.end:
        returns = returns[returns.index <= pd.Timestamp(args.end)]

    bmk = benchmarks.benchmark_returns(
        start=returns.index.min() if not args.start else pd.Timestamp(args.start),
        end=returns.index.max() if not args.end else pd.Timestamp(args.end),
    )
    returns, bmk = align_to_benchmarks(returns, bmk)
    if returns.empty:
        raise RuntimeError("No overlap between strategy and benchmarks")

    report = render_report(
        label, returns, bmk, n_boot=args.n_boot, rng_seed=args.rng_seed
    )

    REPORTS_DIR.mkdir(exist_ok=True)
    out_path = args.output or REPORTS_DIR / f"validation_{label}.md"
    out_path.write_text(report)
    print(f"Wrote {out_path}")
    print(textwrap.shorten(report, width=500, placeholder=" ..."))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
