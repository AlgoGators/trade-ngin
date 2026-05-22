"""
Harvey, Liu, Zhu (2016) multiple-testing haircut for Sharpe ratios.

Paper: "... and the Cross-Section of Expected Returns", RFS 2016.

Given a family of N independent strategy tests, the single-test
p-value must be adjusted. HLZ provide three adjustments:
    - Bonferroni (stringent, independent-test assumption)
    - Holm step-down (less conservative than Bonferroni)
    - Benjamini-Hochberg-Yekutieli (FDR control under dependence)

For Sharpe ratios, they translate the adjusted p into an "implied haircut
Sharpe" — the minimum SR that would still reject the null at alpha = 0.05
after correction. Formulas:
    t_stat = SR_hat * sqrt(T) / sqrt(1 - SR_hat^2 / T + ...)  (Lo 2002)
    p_1    = 2 * (1 - Phi(|t|))                                 single-sided, but
                                                                 SR interpretation
                                                                 uses one-sided
    p_adj  = min(1, p_1 * adjustment_factor)
    SR_implied = Phi^{-1}(1 - p_adj) / sqrt(T) * sqrt(252)     (annualized)
"""

from __future__ import annotations

import numpy as np
from scipy import stats


def lo_sharpe_tstat(sr_hat_periodic: float, t: int) -> float:
    """
    Lo 2002 t-statistic for an annualized Sharpe on iid returns.
    Uses the simpler form: t = SR * sqrt(T).
    Skew/kurtosis adjustment via Mertens is handled separately by dsr.py.
    """
    return float(sr_hat_periodic * np.sqrt(t))


def bonferroni_p(p_single: float, n_trials: int) -> float:
    return float(min(1.0, p_single * n_trials))


def holm_p(p_singles: np.ndarray) -> np.ndarray:
    """
    Holm-Bonferroni step-down adjustment. Sorts singles ascending, multiplies
    the k-th smallest by (N - k + 1). Returns adjusted p's in ORIGINAL order.
    """
    p = np.asarray(p_singles, dtype=float)
    n = p.size
    order = np.argsort(p)
    adjusted = np.zeros(n)
    running = 0.0
    for k, idx in enumerate(order):
        factor = n - k
        running = max(running, min(1.0, p[idx] * factor))
        adjusted[idx] = running
    return adjusted


def bhy_p(p_singles: np.ndarray) -> np.ndarray:
    """
    Benjamini-Hochberg-Yekutieli FDR-control (HLZ 2016 Table 5).
    Valid under arbitrary dependence: includes the harmonic-sum correction
    c(N) = sum_{k=1..N} 1/k.

        p_adj^{(k)} = min_{j >= k} ( p^{(j)} * N * c(N) / j )
    """
    p = np.asarray(p_singles, dtype=float)
    n = p.size
    order = np.argsort(p)
    c_n = float(np.sum(1.0 / np.arange(1, n + 1)))
    adjusted_ordered = np.zeros(n)
    # Traverse largest -> smallest, monotone take-min from the right.
    running_min = 1.0
    for rank_from_bottom in range(n - 1, -1, -1):
        j = rank_from_bottom + 1  # 1-indexed rank
        raw = p[order[rank_from_bottom]] * n * c_n / j
        running_min = min(running_min, min(1.0, raw))
        adjusted_ordered[rank_from_bottom] = running_min
    # Back to original indexing.
    adjusted = np.zeros(n)
    adjusted[order] = adjusted_ordered
    return adjusted


def implied_haircut_sharpe(
    p_adj: float,
    t: int,
    periods_per_year: int = 252,
    two_sided: bool = False,
) -> float:
    """
    Convert an adjusted p-value back to the minimum annualized Sharpe
    that would still be rejected at alpha = p_adj.
    """
    if p_adj <= 0.0 or p_adj >= 1.0:
        return np.nan
    if two_sided:
        z = stats.norm.ppf(1.0 - p_adj / 2.0)
    else:
        z = stats.norm.ppf(1.0 - p_adj)
    return float(z / np.sqrt(t) * np.sqrt(periods_per_year))


def haircut_report(
    sr_hat_annualized: float,
    t: int,
    n_trials: int,
    periods_per_year: int = 252,
) -> dict:
    """
    Produce a one-strategy haircut report (single strategy being tested
    against a family of N prior trials). Returns a dict with:
        single_p, bonferroni_p, bonferroni_sr, holm_p, holm_sr,
        bhy_p, bhy_sr, reject_5pct_{single,bonferroni,holm,bhy}.
    """
    sr_periodic = sr_hat_annualized / np.sqrt(periods_per_year)
    t_stat = lo_sharpe_tstat(sr_periodic, t)
    # One-sided: p = P(Z > t) under the null.
    p_single = float(1.0 - stats.norm.cdf(t_stat))

    # Family-adjusted p's — we place our observed strategy among N
    # implicit tests and correct only the current one. HLZ frame this as
    # "N trials tried, at least one succeeded at SR_hat".
    p_bonf = bonferroni_p(p_single, n_trials)
    # For Holm and BHY we need the full family's p-values. If the user
    # only supplies a single SR, we conservatively assume all N-1 other
    # trials produced p = 0.5 (worst-case dense competition). This
    # matches the HLZ "research universe" framing.
    other = np.full(n_trials - 1, 0.5)
    family = np.concatenate([[p_single], other])
    p_holm = float(holm_p(family)[0])
    p_bhy = float(bhy_p(family)[0])

    return {
        "single_p": p_single,
        "t_stat": t_stat,
        "n_trials": n_trials,
        "bonferroni_p": p_bonf,
        "bonferroni_sr": implied_haircut_sharpe(p_bonf, t, periods_per_year),
        "holm_p": p_holm,
        "holm_sr": implied_haircut_sharpe(p_holm, t, periods_per_year),
        "bhy_p": p_bhy,
        "bhy_sr": implied_haircut_sharpe(p_bhy, t, periods_per_year),
        "reject_5pct_single": p_single < 0.05,
        "reject_5pct_bonferroni": p_bonf < 0.05,
        "reject_5pct_holm": p_holm < 0.05,
        "reject_5pct_bhy": p_bhy < 0.05,
    }


if __name__ == "__main__":
    # At T = 15 years daily, an annualized SR of 0.7 has t ~= 2.7. Under
    # N = 30 Bonferroni, that fails alpha = 0.05 (p*30 ~= 0.21).
    rep = haircut_report(sr_hat_annualized=0.705, t=252 * 15, n_trials=30)
    for k, v in rep.items():
        print(f"  {k:24s} = {v}")
