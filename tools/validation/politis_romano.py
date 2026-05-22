"""
Politis-Romano stationary block bootstrap (1994).

Draws block-length geometric(p) with expected block length 1/p.
Block-length selection defaults to Politis-White 2004 /
Patton-Politis-White 2009 automatic selector (spectral-density-based).

Used here to build a 95 % confidence interval for the sample Sharpe
under serial dependence, and for the Ledoit-Wolf 2008 pairwise Sharpe
test vs a benchmark.
"""

from __future__ import annotations

from typing import Callable

import numpy as np
from scipy import stats


def optimal_block_length(returns: np.ndarray) -> float:
    """
    Politis-White (2004) automatic block-length selection, "sb" version
    for the stationary bootstrap. Returns expected block length 1/p.

    Simplified from Politis-White formulas — uses the rule-of-thumb
    spectral estimator based on autocovariances. Good default when the
    user doesn't want to tune.
    """
    r = np.asarray(returns, dtype=float)
    r = r[np.isfinite(r)]
    n = r.size
    if n < 50:
        return max(1.0, n / 10.0)

    # Autocovariances up to floor(log_2(n)).
    max_lag = max(5, int(np.log2(n)))
    rc = r - r.mean()
    gamma = np.array(
        [np.dot(rc[:n - k], rc[k:]) / n for k in range(max_lag + 1)]
    )

    # Rule: use the lag-0 (variance) and lag-1,2,... autocovariances
    # weighted by flat-top kernel to get the block length. See PW 2004
    # eq. 13-14. Simplified: pick lag M where |gamma_k/gamma_0| drops
    # below 2/sqrt(n) for two consecutive lags.
    thresh = 2.0 / np.sqrt(n)
    m = 1
    for k in range(1, max_lag):
        if (abs(gamma[k]) / abs(gamma[0]) < thresh
                and abs(gamma[k + 1]) / abs(gamma[0]) < thresh):
            m = k
            break
    else:
        m = max_lag

    # PW 2004 eq. 15 for stationary-bootstrap block length.
    g_hat = 2.0 * sum((1 - j / (m + 1)) * gamma[j] for j in range(1, m + 1))
    d_hat = 2.0 * gamma[0] ** 2
    if d_hat == 0.0:
        return max(1.0, float(n ** (1 / 3)))
    b_opt = (2.0 * g_hat ** 2 / d_hat) ** (1 / 3) * (n ** (1 / 3))
    return max(1.0, float(min(b_opt, n / 4)))


def stationary_bootstrap_resample(
    returns: np.ndarray,
    block_length: float,
    rng: np.random.Generator,
) -> np.ndarray:
    """
    One stationary-bootstrap draw of the same length as `returns`.
    Geometric(1/block_length) block lengths, wrap-around indexing.
    """
    r = np.asarray(returns, dtype=float)
    n = r.size
    p = 1.0 / max(block_length, 1.0)

    out = np.empty(n)
    i = 0
    while i < n:
        start = int(rng.integers(0, n))
        # Geometric block length, min 1.
        k = int(rng.geometric(p))
        # Copy up to end of `out`.
        take = min(k, n - i)
        for j in range(take):
            out[i + j] = r[(start + j) % n]
        i += take
    return out


def sharpe_bootstrap_ci(
    returns: np.ndarray,
    n_boot: int = 10000,
    periods_per_year: int = 252,
    ci: float = 0.95,
    block_length: float | None = None,
    rng_seed: int | None = None,
) -> dict:
    """
    95 % stationary-bootstrap CI for the annualized Sharpe ratio.
    """
    r = np.asarray(returns, dtype=float)
    r = r[np.isfinite(r)]
    rng = np.random.default_rng(rng_seed)
    if block_length is None:
        block_length = optimal_block_length(r)

    def sr(x: np.ndarray) -> float:
        if x.std(ddof=1) == 0.0:
            return 0.0
        return float(x.mean() / x.std(ddof=1) * np.sqrt(periods_per_year))

    observed = sr(r)
    draws = np.empty(n_boot)
    for b in range(n_boot):
        sample = stationary_bootstrap_resample(r, block_length, rng)
        draws[b] = sr(sample)

    alpha = (1.0 - ci) / 2.0
    lo = float(np.quantile(draws, alpha))
    hi = float(np.quantile(draws, 1.0 - alpha))
    return {
        "observed_sr": observed,
        "mean_sr": float(draws.mean()),
        "std_sr": float(draws.std(ddof=1)),
        "ci_low": lo,
        "ci_high": hi,
        "block_length": float(block_length),
        "n_boot": n_boot,
    }


def ledoit_wolf_sharpe_test(
    strategy_returns: np.ndarray,
    benchmark_returns: np.ndarray,
    n_boot: int = 10000,
    periods_per_year: int = 252,
    block_length: float | None = None,
    rng_seed: int | None = None,
) -> dict:
    """
    Ledoit-Wolf (2008) studentized block-bootstrap test of
    H0: SR_strategy = SR_benchmark vs H1: SR_strategy > SR_benchmark.
    Returns one-sided p-value. Strategy "wins" when p < 0.05.
    """
    r = np.asarray(strategy_returns, dtype=float)
    b = np.asarray(benchmark_returns, dtype=float)
    # Align to same length — we assume both came from the same backtest
    # window. If shapes differ, truncate to the shorter.
    n = min(r.size, b.size)
    r = r[:n]
    b = b[:n]
    paired = np.isfinite(r) & np.isfinite(b)
    r = r[paired]
    b = b[paired]
    if r.size < 30:
        return {"error": "insufficient_data"}

    rng = np.random.default_rng(rng_seed)
    if block_length is None:
        block_length = optimal_block_length(np.concatenate([r, b]))

    def sr(x: np.ndarray) -> float:
        if x.std(ddof=1) == 0.0:
            return 0.0
        return float(x.mean() / x.std(ddof=1) * np.sqrt(periods_per_year))

    diff_obs = sr(r) - sr(b)
    # Centered bootstrap: resample the pair jointly using index blocks.
    n_obs = r.size

    def resample_pair(seed_rng):
        p = 1.0 / max(block_length, 1.0)
        idx = np.empty(n_obs, dtype=int)
        i = 0
        while i < n_obs:
            start = int(seed_rng.integers(0, n_obs))
            k = int(seed_rng.geometric(p))
            take = min(k, n_obs - i)
            for j in range(take):
                idx[i + j] = (start + j) % n_obs
            i += take
        return r[idx], b[idx]

    draws = np.empty(n_boot)
    for k in range(n_boot):
        rs, bs = resample_pair(rng)
        # Centered difference: subtract observed to approximate null distribution.
        draws[k] = (sr(rs) - sr(bs)) - diff_obs

    # One-sided: H1 is diff > 0, so p = P(draw >= observed | null).
    p_value = float(np.mean(np.abs(draws) >= abs(diff_obs)))
    return {
        "sr_strategy": sr(r),
        "sr_benchmark": sr(b),
        "sr_diff": diff_obs,
        "p_value_two_sided": p_value,
        "p_value_one_sided": float(np.mean(draws >= diff_obs))
        if diff_obs > 0
        else 1.0 - float(np.mean(draws >= diff_obs)),
        "reject_5pct_one_sided": (
            diff_obs > 0 and float(np.mean(draws >= diff_obs)) < 0.05
        ),
        "block_length": float(block_length),
        "n_boot": n_boot,
    }


if __name__ == "__main__":
    rng = np.random.default_rng(42)
    strat = rng.normal(0.8 / np.sqrt(252), 1.0 / np.sqrt(252), size=252 * 15)
    bench = rng.normal(0.4 / np.sqrt(252), 1.0 / np.sqrt(252), size=252 * 15)
    ci = sharpe_bootstrap_ci(strat, n_boot=2000, rng_seed=7)
    print("Sharpe 95% CI:", ci)
    lw = ledoit_wolf_sharpe_test(strat, bench, n_boot=2000, rng_seed=7)
    print("Ledoit-Wolf test:", lw)
