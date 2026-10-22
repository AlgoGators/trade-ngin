"""Deterministic return-series statistics for issue #113 (no database access).

PSR/DSR use *per-period* Sharpes throughout. The DSR expected-maximum hurdle
follows Bailey and Lopez de Prado (2014), Eq. 1-2. CSCV follows Bailey et al.
(2017). These are descriptive diagnostics and do not imply a release decision.
"""

import itertools
import math
import random
import statistics as stat


def _check_returns(values):
    values = list(values)
    if not values or any(not math.isfinite(x) or x <= -1 for x in values):
        raise ValueError("returns must be nonempty, finite, and greater than -1")
    return values


def sharpe(values, periods_per_year=1.0):
    values = _check_returns(values)
    if len(values) < 2:
        return None
    sd = stat.stdev(values)
    if sd == 0:
        return None
    if periods_per_year <= 0:
        raise ValueError("periods_per_year must be positive")
    return stat.mean(values) / sd * math.sqrt(periods_per_year)


def drawdown(values):
    values = _check_returns(values)
    equity = peak = 1.0
    maximum = 0.0
    for value in values:
        equity *= 1 + value
        peak = max(peak, equity)
        maximum = max(maximum, 1 - equity / peak)
    return maximum


def analysis_summary(values, periods_per_year=252.0):
    values = _check_returns(values)
    if periods_per_year <= 0:
        raise ValueError("periods_per_year must be positive")
    total = math.prod(1 + value for value in values)
    return {"periods": len(values), "total_return": total - 1,
            "annual_return": total ** (periods_per_year / len(values)) - 1,
            "sharpe": sharpe(values, periods_per_year),
            "max_drawdown": drawdown(values)}


def moving_block_sample(values, block_length, rng):
    """Circular moving blocks of exactly block_length; final block is truncated."""
    values = _check_returns(values)
    if not 1 <= block_length <= len(values):
        raise ValueError("block_length must be between 1 and the series length")
    out = []
    while len(out) < len(values):
        start = rng.randrange(len(values))
        out.extend(values[(start + step) % len(values)] for step in range(block_length))
    return out[:len(values)]


def _percentile(sorted_values, fraction):
    point = fraction * (len(sorted_values) - 1)
    low = int(point)
    high = min(low + 1, len(sorted_values) - 1)
    return sorted_values[low] + (sorted_values[high] - sorted_values[low]) * (point - low)


def bootstrap_interval(values, statistic, *, block_length, resamples=10000,
                       seed=42, confidence=0.95):
    values = _check_returns(values)
    if resamples < 1 or not 0 < confidence < 1:
        raise ValueError("invalid resamples or confidence")
    rng = random.Random(seed)
    estimates = []
    for _ in range(resamples):
        sample = moving_block_sample(values, block_length, rng)
        estimate = statistic(sample)
        if estimate is not None and math.isfinite(estimate):
            estimates.append(estimate)
    if len(estimates) != resamples:
        raise ValueError("one or more resamples produced an undefined statistic")
    estimates.sort()
    tail = (1 - confidence) / 2
    return (_percentile(estimates, tail), _percentile(estimates, 1 - tail))


def probabilistic_sharpe(values, benchmark_per_period=0.0):
    """PSR against a per-period Sharpe benchmark; skew/kurtosis are raw moments."""
    values = _check_returns(values)
    n = len(values)
    observed = sharpe(values)
    if n < 3 or observed is None:
        raise ValueError("PSR requires at least three nonconstant returns")
    mean = stat.mean(values)
    moments = [stat.mean((x - mean) ** power for x in values) for power in (2, 3, 4)]
    skew = moments[1] / moments[0] ** 1.5
    kurtosis = moments[2] / moments[0] ** 2
    variance_factor = 1 - skew * observed + (kurtosis - 1) * observed ** 2 / 4
    if variance_factor <= 0:
        raise ValueError("PSR variance approximation is nonpositive")
    z = (observed - benchmark_per_period) * math.sqrt(n - 1) / math.sqrt(variance_factor)
    return stat.NormalDist().cdf(z)


def deflated_sharpe(selected_returns, trial_sharpes_per_period):
    """DSR using every trial's per-period Sharpe and the raw trial count."""
    trial_sharpes_per_period = list(trial_sharpes_per_period)
    if not trial_sharpes_per_period or any(
            value is None or not math.isfinite(value) for value in trial_sharpes_per_period):
        raise ValueError("DSR requires a finite Sharpe for every tried configuration")
    count = len(trial_sharpes_per_period)
    if count == 1:
        hurdle = 0.0
    else:
        spread = stat.stdev(trial_sharpes_per_period)
        gamma = 0.5772156649015329
        normal = stat.NormalDist()
        hurdle = spread * ((1 - gamma) * normal.inv_cdf(1 - 1 / count)
                           + gamma * normal.inv_cdf(1 - 1 / (count * math.e)))
    return probabilistic_sharpe(selected_returns, hurdle)


def cscv_pbo(matrix, groups=10):
    """CSCV over a real T×N matrix, selecting IS Sharpe and ranking OOS Sharpe."""
    if len(matrix) < 2 or groups < 2 or groups % 2:
        raise ValueError("CSCV requires at least two trials and an even group count")
    ids = sorted(matrix)
    series = {key: _check_returns(matrix[key]) for key in ids}
    length = len(series[ids[0]])
    if any(len(values) != length for values in series.values()):
        raise ValueError("matrix columns must share a complete date index")
    if length % groups or length // groups < 2:
        raise ValueError("CSCV needs equal groups with at least two returns each")
    size = length // groups
    logits = []
    ties = 0
    for training_groups in itertools.combinations(range(groups), groups // 2):
        selected_groups = set(training_groups)
        training = [i for i in range(length) if i // size in selected_groups]
        testing = [i for i in range(length) if i // size not in selected_groups]
        insample = {key: sharpe([series[key][i] for i in training]) for key in ids}
        outsample = {key: sharpe([series[key][i] for i in testing]) for key in ids}
        if any(value is None for value in itertools.chain(insample.values(), outsample.values())):
            raise ValueError("CSCV slice has undefined Sharpe; report the slice, do not fill it")
        winner = sorted(ids, key=lambda key: (-insample[key], key))[0]
        selected_score = outsample[winner]
        below = sum(value < selected_score for value in outsample.values())
        equal = sum(value == selected_score for value in outsample.values())
        if equal > 1 or list(insample.values()).count(insample[winner]) > 1:
            ties += 1
        rank = below + (equal + 1) / 2  # average 1-based rank for ties
        relative_rank = rank / (len(ids) + 1)
        logits.append(math.log(relative_rank / (1 - relative_rank)))
    return {"pbo": sum(value < 0 for value in logits) / len(logits),
            "combinations": len(logits), "ties": ties, "logits": logits}
