"""Manifest-backed, offline issue #113 evaluation of immutable scratch exports."""

import datetime as dt
import calendar
import hashlib
import math
import pathlib
import random
import re

from .curve import CurveError, compare_prefix, read_curve, scored_returns
from .statistics import (analysis_summary, cscv_pbo, deflated_sharpe,
                         moving_block_sample, probabilistic_sharpe, sharpe)


class WorkflowError(ValueError):
    """The evidence contract is incomplete or inconsistent."""


def _file_for(run, base_dir):
    path = pathlib.Path(run["curve_csv"])
    return path if path.is_absolute() else pathlib.Path(base_dir) / path


def _verified_file(run, base_dir):
    path = _file_for(run, base_dir)
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != run.get("curve_sha256"):
        raise WorkflowError("curve checksum differs for " + run["run_id"])
    return path


def load_matrix(manifest, base_dir):
    """Produce one shared daily date index and genuine tried-config columns."""
    dates = None
    matrix = {}
    missing = []
    seen_trials = set()
    for run in manifest["runs"]:
        if run.get("role", "trial") == "causality_only":
            continue
        trial = run["trial_id"]
        if trial in seen_trials:
            raise WorkflowError("duplicate trial_id: " + trial)
        seen_trials.add(trial)
        if run.get("missing_reason"):
            missing.append({"trial_id": trial, "reason": run["missing_reason"]})
            continue
        try:
            path = _verified_file(run, base_dir)
            curve = read_curve(path, run["run_id"], run["portfolio_id"])
            returns = scored_returns(curve, run["warmup_last_timestamp"],
                                     run["scored_start"], run["scored_end"])
        except (KeyError, OSError, CurveError) as exc:
            raise WorkflowError("invalid export for " + trial + ": " + str(exc)) from exc
        current_dates = [timestamp.date() for timestamp, _ in returns]
        if len(set(current_dates)) != len(current_dates):
            raise WorkflowError("more than one equity row per UTC date for " + trial)
        if dates is None:
            dates = current_dates
        elif current_dates != dates:
            raise WorkflowError("trial date index differs; no silent intersection: " + trial)
        matrix[trial] = [value for _, value in returns]
    if not matrix:
        raise WorkflowError("no comparable scored trial exports")
    return dates, matrix, missing


def _check_causality(manifest, base_dir, gate):
    if gate.get("status") != "pass" or gate.get("matched_rows", 0) < 1 or gate.get("first_difference"):
        raise WorkflowError("causality gate did not pass")
    curves = {}
    run_metadata = {}
    for label in ("short", "long"):
        run_id = gate.get(label + "_run")
        matching = [run for run in manifest["runs"] if run["run_id"] == run_id]
        if len(matching) != 1:
            raise WorkflowError("causality " + label + " run is absent or ambiguous")
        run_metadata[label] = matching[0]
        path = _verified_file(matching[0], base_dir)
        if gate.get(label + "_sha256") != hashlib.sha256(path.read_bytes()).hexdigest():
            raise WorkflowError("causality gate checksum differs for " + label)
        if matching[0]["portfolio_id"] != gate.get("portfolio_id"):
            raise WorkflowError("causality gate portfolio differs")
        curves[label] = read_curve(path, run_id, gate["portfolio_id"])
    short, long = run_metadata["short"], run_metadata["long"]
    end = dt.date.fromisoformat(gate["end_date"])
    next_year = end.replace(year=end.year + 1,
                            day=min(end.day, calendar.monthrange(end.year + 1, end.month)[1]))
    if (short.get("window_start") != long.get("window_start")
            or short.get("window_end") != end.isoformat()
            or long.get("window_end") != next_year.isoformat()
            or curves["short"][-1].timestamp.date() > end
            or curves["long"][-1].timestamp.date() <= end):
        raise WorkflowError("causality window metadata or actual extension is invalid")
    protocol = manifest.get("window_protocol")
    if protocol == "frozen_end_date_plus_lookback_years":
        if (not isinstance(short.get("lookback_years"), int)
                or long.get("lookback_years") != short["lookback_years"] + 1):
            raise WorkflowError("causality lookback window does not extend by one year")
    elif protocol != "explicit_start_end":
        raise WorkflowError("unknown causality window protocol")
    recomputed = compare_prefix(curves["short"], curves["long"], gate["end_date"])
    if recomputed["status"] != "pass" or recomputed["matched_rows"] != gate["matched_rows"]:
        raise WorkflowError("causality gate differs from the archived curves")


def _percentile(sorted_values, fraction):
    point = fraction * (len(sorted_values) - 1)
    low = int(point)
    return sorted_values[low] + (sorted_values[min(low + 1, len(sorted_values) - 1)]
                                 - sorted_values[low]) * (point - low)


def _validate_settings(settings):
    blocks = settings["block_lengths"]
    if (len(blocks) != 3 or any(not isinstance(x, int) or x < 1 for x in blocks)
            or blocks != sorted(set(blocks))):
        raise WorkflowError("block_lengths must be three increasing positive integers")
    if (not isinstance(settings["resamples"], int) or settings["resamples"] < 1
            or not isinstance(settings["seed"], int)
            or settings["periods_per_year"] <= 0
            or not 0 < settings["confidence"] < 1
            or not isinstance(settings["cscv_groups"], int)
            or settings["cscv_groups"] < 2 or settings["cscv_groups"] % 2):
        raise WorkflowError("invalid preregistered analysis settings")


def _intervals(values, settings, full_drawdown=False):
    n = len(values)
    result = {}
    lengths = [1] + list(settings["block_lengths"])
    for length in dict.fromkeys(lengths):
        if length > n:
            result[str(length)] = {"status": "insufficient_rows", "rows": n}
            continue
        rng = random.Random(settings["seed"])
        samples = {"sharpe": [], "annual_return": [], "max_drawdown": []}
        for _ in range(settings["resamples"]):
            sample = moving_block_sample(values, length, rng)
            summary = analysis_summary(sample, settings["periods_per_year"])
            for key in samples:
                value = summary[key]
                if value is not None and math.isfinite(value):
                    samples[key].append(value)
        lower = (1 - settings["confidence"]) / 2
        report = {"method": "iid" if length == 1 else "circular_moving_block",
                  "block_length": length, "resamples": settings["resamples"],
                  "intervals": {}}
        for key, observations in samples.items():
            observations.sort()
            report["intervals"][key] = (
                [_percentile(observations, lower), _percentile(observations, 1 - lower)]
                if observations else None)
            report["intervals"][key + "_valid_resamples"] = len(observations)
        if full_drawdown and length == settings["block_lengths"][1]:
            report["drawdown_distribution"] = samples["max_drawdown"]
        result[str(length)] = report
    return result


def offline_selection(dates, matrix, fixed_trial):
    """Select on the previous calendar year; evaluate on stored next-year returns."""
    if fixed_trial not in matrix:
        raise WorkflowError("fixed trial absent from matrix")
    years = sorted({date.year for date in dates})
    result = []
    for prior, next_year in zip(years, years[1:]):
        if next_year != prior + 1:
            continue
        train = [i for i, date in enumerate(dates) if date.year == prior]
        test = [i for i, date in enumerate(dates) if date.year == next_year]
        scores = {trial: sharpe([column[i] for i in train])
                  for trial, column in matrix.items()}
        if any(value is None for value in scores.values()):
            result.append({"selection_year": prior, "test_year": next_year,
                           "status": "undefined_training_sharpe"})
            continue
        chosen = sorted(scores, key=lambda trial: (-scores[trial], trial))[0]
        selected = [matrix[chosen][i] for i in test]
        fixed = [matrix[fixed_trial][i] for i in test]
        result.append({"selection_year": prior, "test_year": next_year,
                       "status": "computed", "selected_trial": chosen,
                       "selected_training_sharpe": scores[chosen],
                       "selected_total_return": math.prod(1 + x for x in selected) - 1,
                       "fixed_total_return": math.prod(1 + x for x in fixed) - 1,
                       "test_rows": len(test)})
    return result


def analyze(manifest, base_dir, gate, fixed_trial):
    """Return evidence JSON; a synthetic manifest is never a real strategy report."""
    if manifest.get("issue") != 113 or not re.fullmatch(r"[0-9a-f]{40}",
                                                       manifest.get("engine_sha", "")):
        raise WorkflowError("manifest lacks issue 113 and a full engine SHA")
    try:
        snapshot = dt.datetime.fromisoformat(manifest["snapshot_utc"].replace("Z", "+00:00"))
        if snapshot.utcoffset() != dt.timedelta(0):
            raise WorkflowError("snapshot_utc must include the UTC offset")
        settings = manifest["analysis"]
        _validate_settings(settings)
        _check_causality(manifest, base_dir, gate)
        dates, matrix, missing = load_matrix(manifest, base_dir)
    except (KeyError, OSError, ValueError) as exc:
        if isinstance(exc, WorkflowError):
            raise
        raise WorkflowError("manifest or evidence invalid: " + str(exc)) from exc
    if fixed_trial not in matrix:
        raise WorkflowError("fixed trial absent from comparable exports")
    primary = settings["block_lengths"][1]
    values = matrix[fixed_trial]
    annual = {}
    for year in sorted({date.year for date in dates}):
        year_values = [value for date, value in zip(dates, values) if date.year == year]
        annual[str(year)] = {"point": analysis_summary(year_values, settings["periods_per_year"]),
                             "intervals": _intervals(year_values, settings)}
    prefixes = []
    for year in sorted({date.year for date in dates}):
        prefix = [value for date, value in zip(dates, values) if date.year <= year]
        interval = _intervals(prefix, settings)[str(primary)]["intervals"].get("sharpe")
        prefixes.append({"through_year": year, "scored_days": len(prefix),
                         "sharpe_interval": interval,
                         "excludes_zero": bool(interval and (interval[0] > 0 or interval[1] < 0))})
    trial_count = sum(run.get("role", "trial") == "trial" for run in manifest["runs"])
    complete = bool(manifest.get("trial_ledger_complete") and not missing
                    and trial_count == len(matrix))
    selection = {"trial_count": trial_count, "ledger_complete": complete,
                 "missing_trials": missing, "pbo": None, "dsr": None}
    if len(matrix) >= 2:
        groups = settings["cscv_groups"]
        trimmed = len(dates) % groups
        usable = len(dates) - trimmed
        selection["pbo_dropped_tail_dates"] = [date.isoformat() for date in dates[usable:]]
        if usable // groups >= 2:
            selection["pbo"] = cscv_pbo(
                {trial: column[:usable] for trial, column in matrix.items()}, groups)
            selection["pbo"]["scope"] = "full_trial_ledger" if complete else "demonstration_only"
    if complete:
        trial_sharpes = [sharpe(column) for column in matrix.values()]
        selection["dsr"] = deflated_sharpe(values, trial_sharpes)
    return {"issue": 113, "engine_sha": manifest["engine_sha"],
            "snapshot_utc": manifest["snapshot_utc"],
            "causality": gate, "fixed_trial": fixed_trial,
            "scored_start": dates[0].isoformat(), "scored_end": dates[-1].isoformat(),
            "scored_rows": len(dates),
            "fixed": {"point": analysis_summary(values, settings["periods_per_year"]),
                      "intervals": _intervals(values, settings, full_drawdown=True),
                      "psr_vs_zero": probabilistic_sharpe(values, 0.0)},
            "calendar_years": annual, "exploratory_prefixes": prefixes,
            "selection_risk": selection,
            "offline_next_year": offline_selection(dates, matrix, fixed_trial)}
