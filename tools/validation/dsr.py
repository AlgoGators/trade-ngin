"""
Deflated Sharpe Ratio (Bailey & Lopez de Prado 2014).

The DSR answers: given the observed Sharpe, the number of strategy
trials implied by the search, and the return distribution's non-normal
shape (skew, kurtosis), what is the probability this Sharpe is above
the false-discovery null at alpha = 0.05?

Usage:
    dsr, sr_hat, sr_null = deflated_sharpe(
        returns,           # pd.Series or np.ndarray of periodic returns
        n_trials=30,       # independent strategies tried (Harvey-Liu default)
        periods_per_year=252,
    )
    # dsr >= 0.95 => reject the null at alpha = 0.05.
"""

from __future__ import annotations

import numpy as np
from scipy import stats

EULER_MASCHERONI = 0.5772156649015329


def sharpe_ratio(returns: np.ndarray, periods_per_year: int = 252) -> float:
    """Annualized Sharpe of periodic returns."""
    r = np.asarray(returns, dtype=float)
    r = r[np.isfinite(r)]
    if r.size < 2 or r.std(ddof=1) == 0.0:
        return 0.0
    return float(r.mean() / r.std(ddof=1) * np.sqrt(periods_per_year))


def expected_max_sharpe(n_trials: int, sr_variance: float) -> float:
    """
    Expected max Sharpe of N iid trials with per-trial SR variance `sr_variance`,
    assuming SR estimates are normally distributed around zero (pure
    false-discovery null). Bailey-Lopez de Prado 2014 eq. 11.

    E[max_N(SR)] ~ sqrt(V[SR]) * ( (1 - gamma) * Phi^{-1}(1 - 1/N)
                                  + gamma       * Phi^{-1}(1 - 1/(N*e)) )
    with gamma = Euler-Mascheroni.
    """
    if n_trials < 2:
        return 0.0
    inv = stats.norm.ppf
    e = np.e
    return float(
        np.sqrt(sr_variance)
        * (
            (1 - EULER_MASCHERONI) * inv(1.0 - 1.0 / n_trials)
            + EULER_MASCHERONI * inv(1.0 - 1.0 / (n_trials * e))
        )
    )


def sr_variance(returns: np.ndarray, periods_per_year: int = 252) -> float:
    """
    Annualized sample variance of the Sharpe-ratio estimator under skew
    and excess kurtosis. Mertens 2002 closed form used by BLdP 2014:

        V[SR_hat] = (1 - gamma_3 * SR + (gamma_4 - 1)/4 * SR^2) / (T - 1)

    with gamma_3 = skewness, gamma_4 = kurtosis (NOT excess).
    """
    r = np.asarray(returns, dtype=float)
    r = r[np.isfinite(r)]
    t = r.size
    if t < 4:
        return 1.0
    sr = sharpe_ratio(r, periods_per_year)
    # SR at periodic (daily) scale for Mertens formula.
    sr_periodic = sr / np.sqrt(periods_per_year)
    g3 = float(stats.skew(r, bias=False))
    g4 = float(stats.kurtosis(r, fisher=False, bias=False))  # non-excess kurtosis
    var_sr_periodic = (
        1.0 - g3 * sr_periodic + (g4 - 1.0) / 4.0 * sr_periodic**2
    ) / (t - 1)
    return float(var_sr_periodic * periods_per_year)


def deflated_sharpe(
    returns: np.ndarray,
    n_trials: int = 30,
    periods_per_year: int = 252,
) -> tuple[float, float, float]:
    """
    Return (DSR probability, observed Sharpe, null-max Sharpe).

    DSR = P(true SR > 0 | observed SR, N trials, T observations, skew,
    kurtosis). BLdP 2014 eq. 15:

        DSR = Phi( (SR_hat - E[max SR]) * sqrt((T-1) / V[SR_hat]) )

    where V[SR_hat] is Mertens-adjusted for skew/kurtosis.
    """
    r = np.asarray(returns, dtype=float)
    r = r[np.isfinite(r)]
    t = r.size
    if t < 30:
        return (np.nan, np.nan, np.nan)
    sr_hat = sharpe_ratio(r, periods_per_year)
    v_sr = sr_variance(r, periods_per_year)
    sr_null = expected_max_sharpe(n_trials, v_sr)
    if v_sr <= 0.0:
        return (np.nan, sr_hat, sr_null)
    z = (sr_hat - sr_null) * np.sqrt((t - 1) / v_sr)
    dsr = float(stats.norm.cdf(z))
    return (dsr, sr_hat, sr_null)


if __name__ == "__main__":
    # Sanity check: run on a simulated track record where the true SR is 1.0,
    # observed over 15 years of daily data. DSR should be > 0.95 for N=10,
    # and may fail at N=100.
    rng = np.random.default_rng(42)
    t = 252 * 15
    daily = rng.normal(1.0 / np.sqrt(252), 1.0 / np.sqrt(252), size=t)
    for n in (10, 30, 100, 1000):
        dsr, sr, sr_null = deflated_sharpe(daily, n_trials=n)
        print(f"N={n:>5d}  SR_hat={sr:.3f}  SR_null={sr_null:.3f}  DSR={dsr:.3f}")
