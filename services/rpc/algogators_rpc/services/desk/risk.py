"""The desk risk check (contract C9): what the engine's last run of a day's qt book says about the
risk limits, and whether the desk's positions breach them.

Read only. A desk save already runs the engine's whole pass on the desk's book (`--desk`; the
one pass sizes it against the overlay and the per-name cap) and stores what it did in
trading.live_results.risk_detail of the day's qt row, key `desk`:

    {"mode": "desk" | "override", "audit_id": N,
     "symbols": [{"symbol", "asked", "given", "pass_given", "moved_by"}, ...],
     "overlay": {"ran", "refused", "m", "binding", "risk_scale",
                 "over_limit_after_rounding", "over_limit_by_hold"},
     "report_only": true}                       # an approved override: the ask trades as asked

This module reads that record and names the breaches. A breach is a risk limit that bound on the
desk's book (Dom, 2026-10-14):

* a symbol moved by a risk step: `cap` (the per-name cap), `overlay` (the overlay's multiplier m:
  R, R_jump, R_shock, gross or net leverage), `clip` (to the cap) or `trim` (an over-limit book
  trimmed);
* the overlay cut the book (m < 1 with a binding term);
* the stored book still reads over a limit (`over_limit_after_rounding`, `over_limit_by_hold`).

The trading rules are reported but are not breaches: `sign_close` (a long is not turned short,
or the reverse, in one day), `rounding`, `buffer`, `search` and `hold`. An approved override is
reported with its breaches but does not block: the ask was approved past them (D4, D5).

What blocks publishing is narrower (decision 5, HD 2026-10-16; Dom 2026-10-16): only the final
book C over one of the caps in BLOCKING_CAPS (gross leverage, net leverage, the per-name cap and
the fund's margin cap). Everything else, a bound limit, the covariance terms R/R_jump/R_shock, the
trim and every trading rule, is reported and does not block. On an approved override the VP or
President has approved the book past gross, net and per-name, never past the margin cap
(decision 10). `publish_blockers` is that gate; every publish path uses it.

Every check is also reported on its own, breach or not (`checks`, desk.proto 1.4.0): each risk
limit with the stored book's reading and its max, as the engine measured them on the save
(`desk.checks`, written since the 2026-10-14 engine). The labels, units and maxes are trade-ngin's;
AlgoLens renders them as given. A day the engine did not measure (the model's book, a refusal, a
row written before the checks existed) reports the configured maxes with no reading.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, List, Mapping, Optional, Sequence, Tuple

RISK_STEPS = frozenset({"cap", "overlay", "clip", "trim"})
RULE_STEPS = frozenset({"sign_close", "rounding", "buffer", "search", "hold"})
RISK, RULE, NONE = "risk", "rule", "none"

NO_BOOK, MODEL, DESK, OVERRIDE = "no_book", "model", "desk", "override"
# A qt row with no book_source: a desk or override run cleared it and did not finish (the engine
# refuses to publish it; the desk saves again, or the 10:00 fallback restores the model's book).
INCOMPLETE = "incomplete"

# What each overlay term limits, for the report.
TERMS = {
    "R": "portfolio risk (R_max)",
    "R_jump": "jump risk (R_jump_max)",
    "R_shock": "shock risk (R_shock_max)",
    "L_g": "gross leverage (max_gross_leverage)",
    "L_n": "net leverage (max_net_leverage)",
    "CAP": "per-name cap (per_name_cap)",
}
# The carver module's limits the report lists, in order.
LIMIT_KEYS = ("max_gross_leverage", "max_net_leverage", "R_max", "R_jump_max", "R_shock_max",
              "per_name_cap", "trim_max")


# The caps whose breach by the final book blocks publishing (decision 5). An approved override
# waives every one of them except MARGIN (decision 10).
BLOCKING_CAPS = ("L_g", "L_n", "CAP", "MARGIN")
OVERRIDABLE_CAPS = ("L_g", "L_n", "CAP")


@dataclass(frozen=True)
class SymbolRisk:
    symbol: str
    asked: float
    given: float
    moved_by: str
    category: str  # risk | rule | none


@dataclass(frozen=True)
class Breach:
    term: str
    detail: str


# Status of one check.
WITHIN, BOUND, OVER, NOT_MEASURED = "within", "bound", "over", "not_measured"
# Units of a check's value and max; the client formats them.
PERCENT, MULTIPLE, CONTRACTS, TAU, USD = "percent", "multiple", "contracts", "tau", "usd"

# The checks, in report order: term, label, the risk.json key of its configured max.
CHECKS = (
    ("R", "Portfolio risk (R)", "R_max"),
    ("R_jump", "Jump risk (R_jump)", "R_jump_max"),
    ("R_shock", "Shock risk (R_shock)", "R_shock_max"),
    ("L_g", "Gross leverage (L_g)", "max_gross_leverage"),
    ("L_n", "Net leverage (L_n)", "max_net_leverage"),
    ("CAP", "Largest single name (per-name cap)", "per_name_cap"),
    ("TRIM", "Contracts trimmed (trim max)", "trim_max"),
)
COVARIANCE_TERMS = ("R", "R_jump", "R_shock")


@dataclass(frozen=True)
class RiskCheck:
    term: str
    label: str
    value: Optional[float]
    max: Optional[float]
    unit: str
    status: str  # within | bound | over | not_measured
    detail: str


@dataclass(frozen=True)
class DeskRiskRow:
    """The day's qt row of trading.live_results, as the store reads it."""

    book_source: Optional[str] = None
    risk_detail: Optional[Mapping[str, Any]] = None
    gross_leverage: Optional[float] = None
    net_leverage: Optional[float] = None
    margin_cushion: Optional[float] = None
    equity_to_margin: Optional[float] = None
    risk_scale: Optional[float] = None  # live_results.risk_scale of the stored book
    # Contract C11: the stored book's initial margin (margin_posted) and the fund's equity after
    # today's transaction costs (current_portfolio_value).
    initial_margin: Optional[float] = None
    equity: Optional[float] = None


@dataclass(frozen=True)
class RiskMultiplier:
    """One multiplier of the day's book (desk.proto 1.5.0): what the engine applied, or what it is
    configured to apply."""

    name: str
    label: str
    value: Optional[float]
    detail: str


@dataclass(frozen=True)
class DeskRiskReport:
    state: str  # no_book | model | desk | override
    audit_id: int = 0
    within_limits: bool = True
    approved_override: bool = False
    breaches: Sequence[Breach] = field(default_factory=tuple)
    m: float = 1.0
    binding_term: str = "none"
    over_limit_after_rounding: str = ""
    over_limit_by_hold: str = ""
    refused: bool = False
    symbols: Sequence[SymbolRisk] = field(default_factory=tuple)
    gross_leverage: Optional[float] = None
    net_leverage: Optional[float] = None
    margin_cushion: Optional[float] = None
    equity_to_margin: Optional[float] = None
    limits: Sequence[Tuple[str, float]] = field(default_factory=tuple)
    checks: Sequence[RiskCheck] = field(default_factory=tuple)


def category(moved_by: Optional[str]) -> str:
    """risk, rule or none for one symbol's moved_by (a comma list when several sleeves differ)."""
    steps = {s.strip() for s in (moved_by or "").split(",") if s.strip()}
    if steps & RISK_STEPS:
        return RISK
    if steps - {"none"}:
        return RULE
    return NONE


def _terms(text: Optional[str]) -> List[str]:
    return [t for t in (text or "").replace(" ", ";").split(";") if t]


def _number(value: Any, default: float = 0.0) -> float:
    try:
        x = float(value)
    except (TypeError, ValueError):
        return default
    return x if math.isfinite(x) else default


def _describe(term: str) -> str:
    return TERMS.get(term, term)


def derive_desk_risk(row: Optional[DeskRiskRow],
                     limits: Sequence[Tuple[str, float]] = (),
                     tau: Optional[float] = None) -> DeskRiskReport:
    """The risk report of a day's qt book. `row` is None when the day has no qt row yet. `tau`
    (the first live sleeve's risk target, load_tau) puts the covariance limits in percent when
    the engine measured nothing, as on a desk book (D-80)."""
    if row is None:
        return DeskRiskReport(state=NO_BOOK, limits=tuple(limits),
                              checks=configured_checks(limits, None, None, "no book yet", tau)
                              + (fund_cap_check(None),))
    readings = dict(gross_leverage=row.gross_leverage, net_leverage=row.net_leverage,
                    margin_cushion=row.margin_cushion, equity_to_margin=row.equity_to_margin,
                    limits=tuple(limits))
    detail = row.risk_detail if isinstance(row.risk_detail, Mapping) else {}
    desk = detail.get("desk") if isinstance(detail.get("desk"), Mapping) else None
    source = (row.book_source or "").strip().lower()
    if not source:
        return DeskRiskReport(state=INCOMPLETE, checks=configured_checks(
            limits, row.gross_leverage, row.net_leverage,
            "the book is incomplete: a desk or override run did not finish writing it", tau)
            + (fund_cap_check(row),), within_limits=False, **readings)
    if desk is None or source not in (DESK, OVERRIDE):
        # The model's book: the engine's own run, inside its limits by construction.
        return DeskRiskReport(state=MODEL, checks=configured_checks(
            limits, row.gross_leverage, row.net_leverage, "measured on a desk save", tau)
            + (fund_cap_check(row),), **readings)

    overlay = desk.get("overlay") if isinstance(desk.get("overlay"), Mapping) else {}
    m = _number(overlay.get("m"), 1.0)
    binding = str(overlay.get("binding") or "none")
    after_rounding = str(overlay.get("over_limit_after_rounding") or "")
    by_hold = str(overlay.get("over_limit_by_hold") or "")
    refused = bool(overlay.get("refused"))

    symbols = []
    for s in desk.get("symbols") or ():
        if not isinstance(s, Mapping) or not s.get("symbol"):
            continue
        moved_by = str(s.get("moved_by") or "none")
        symbols.append(SymbolRisk(symbol=str(s["symbol"]), asked=_number(s.get("asked")),
                                  given=_number(s.get("given")), moved_by=moved_by,
                                  category=category(moved_by)))
    symbols.sort(key=lambda s: s.symbol)

    breaches: List[Breach] = []
    if m < 1.0 - 1e-9 and binding != "none":
        breaches.append(Breach(binding, f"{_describe(binding)} bound: the positions were cut to "
                                        f"{m:.0%} (the ask was {1 / m - 1:.0%} over)"
                               if m > 0 else f"{_describe(binding)} bound: the book was cut to 0"))
    for term in _terms(after_rounding):
        breaches.append(Breach(term, f"{_describe(term)}: the stored book is still over the limit "
                                     "after rounding"))
    for term in _terms(by_hold):
        breaches.append(Breach(term, f"{_describe(term)}: over the limit through a held position"))
    for s in symbols:
        if s.category == RISK:
            breaches.append(Breach(s.moved_by, f"{s.symbol}: asked {s.asked:g}, the {s.moved_by} "
                                               f"step gave {s.given:g}"))
    if refused:
        breaches.append(Breach("refused", "the overlay could not size the book; the held book "
                                          "was kept"))

    approved_override = source == OVERRIDE or bool(desk.get("report_only"))
    measured = desk.get("checks") if isinstance(desk.get("checks"), Mapping) else None
    if measured is not None:
        checks = measured_checks(measured, binding if m < 1.0 - 1e-9 else "none",
                                 _terms(after_rounding), _terms(by_hold), symbols)
    else:
        why = "the overlay refused the book" if refused else "not recorded by this engine run"
        checks = configured_checks(limits, row.gross_leverage, row.net_leverage, why, tau)
    cap = fund_cap_check(row)
    checks = checks + (cap,)
    if cap.status == OVER:
        breaches.append(Breach("MARGIN", cap.detail))
    if refused:
        checks = checks + (RiskCheck("refused", "Overlay sized the book", None, None, "", OVER,
                                     "the overlay could not size the book; the held book was "
                                     "kept"),)
    return DeskRiskReport(
        checks=checks,
        state=OVERRIDE if approved_override else DESK,
        audit_id=int(_number(desk.get("audit_id"))),
        within_limits=not breaches,
        approved_override=approved_override,
        breaches=tuple(breaches),
        m=m, binding_term=binding, over_limit_after_rounding=after_rounding,
        over_limit_by_hold=by_hold, refused=refused, symbols=tuple(symbols), **readings)


def load_tau(portfolio_dir: str) -> Optional[float]:
    """The first live sleeve's risk target (portfolio.json strategies, the first with
    enabled_live; config.risk_target): the tau the overlay's R limits are ratios to. None when
    the file or the value is missing."""
    path = Path(portfolio_dir) / "portfolio.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    strategies = data.get("strategies") if isinstance(data, Mapping) else None
    for sleeve in (strategies or {}).values() if isinstance(strategies, Mapping) else ():
        if isinstance(sleeve, Mapping) and sleeve.get("enabled_live"):
            config = sleeve.get("config") if isinstance(sleeve.get("config"), Mapping) else {}
            value = _optional(config.get("risk_target"))
            return value if value is not None and value > 0 else None
    return None


def load_limits(portfolio_dir: str) -> List[Tuple[str, float]]:
    """The carver module's limits from <portfolio_dir>/risk.json, in LIMIT_KEYS order; [] when the
    file or the module is missing (the report then shows none, it does not guess)."""
    path = Path(portfolio_dir) / "risk.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    modules = data.get("modules") if isinstance(data, Mapping) else None
    carver = next((m for m in modules or () if isinstance(m, Mapping)
                   and m.get("type") == "carver"), None)
    if carver is None:
        return []
    out = []
    for key in LIMIT_KEYS:
        if key in carver:
            value = _number(carver[key], float("nan"))
            if math.isfinite(value):
                out.append((key, value))
    return out


def _status_of(value: Optional[float], limit: Optional[float]) -> str:
    if value is None:
        return NOT_MEASURED
    if limit is not None and abs(value) > limit + 1e-9:
        return OVER
    return WITHIN


def _optional(value: Any) -> Optional[float]:
    if value is None or isinstance(value, bool):
        return None
    x = _number(value, float("nan"))
    return x if math.isfinite(x) else None


def measured_checks(measured: Mapping[str, Any], binding: str, after_rounding: Sequence[str],
                    by_hold: Sequence[str], symbols: Sequence[SymbolRisk]) -> Tuple[RiskCheck, ...]:
    """The engine's readings of the stored book against the limits in force (desk.checks)."""
    tau = _optional(measured.get("tau"))
    moved = {step.strip() for s in symbols for step in s.moved_by.split(",")}
    out = []
    for term, label, _ in CHECKS:
        entry = measured.get(term) if isinstance(measured.get(term), Mapping) else {}
        value, limit = _optional(entry.get("value")), _optional(entry.get("max"))
        status = _status_of(value, limit)
        notes = []
        if term in COVARIANCE_TERMS:
            unit = PERCENT
            ratio = _optional(entry.get("max_ratio"))
            if ratio is not None and tau is not None:
                notes.append(f"max {ratio:g} x risk target {tau:.0%}")
            if value is None:
                notes.append("not measured: the covariance window is blind")
        elif term == "CAP":
            unit = MULTIPLE
            if entry.get("symbol"):
                notes.append(f"{entry['symbol']}, notional / sizing capital")
            if moved & {"cap", "clip"}:
                status = BOUND
                notes.append("the cap cut a symbol")
        elif term == "TRIM":
            unit = CONTRACTS
            if value and value > 0:
                status = BOUND
                notes.append("the trim removed contracts to fit the limits")
        else:
            unit = MULTIPLE
        if term == binding:
            status = BOUND
            notes.append("the overlay cut the positions to fit this limit")
        if term in after_rounding:
            status = OVER
            notes.append("still over after rounding")
        if term in by_hold:
            status = OVER
            notes.append("over through a held position")
        out.append(RiskCheck(term, label, value, limit, unit, status, "; ".join(notes)))
    return tuple(out)


def configured_checks(limits: Sequence[Tuple[str, float]], gross: Optional[float],
                      net: Optional[float], why: str,
                      tau: Optional[float] = None) -> Tuple[RiskCheck, ...]:
    """The configured maxes when the engine measured nothing: the leverage of the stored book
    (trading.live_results) where the row has it, the rest not measured."""
    configured = dict(limits)
    out = []
    for term, label, key in CHECKS:
        limit = configured.get(key)
        value, unit, detail = None, MULTIPLE, why
        if term in COVARIANCE_TERMS:
            if tau is not None and limit is not None:
                # D-80: in percent, as the engine's measured checks give them on a desk book.
                unit, detail = PERCENT, f"{detail}; max {limit:g} x risk target {tau:.0%}"
                limit = limit * tau
            else:
                unit = TAU  # a multiple of the risk target until the engine measures the book
        elif term == "TRIM":
            unit = CONTRACTS
        elif term == "L_g" and gross is not None:
            value, detail = gross, "the stored book"
        elif term == "L_n" and net is not None:
            value, detail = net, "the stored book"
        out.append(RiskCheck(term, label, value, limit, unit, _status_of(value, limit), detail))
    return tuple(out)


def run_multipliers(report: DeskRiskReport, row: Optional[DeskRiskRow]) -> List[RiskMultiplier]:
    """The multipliers the engine's last run of the day's book applied: the overlay's m (the cut to
    fit the limits; 1 = no cut) and the risk scale (the stored book over the target)."""
    if row is None:
        return [RiskMultiplier("m", "Risk overlay multiplier (m)", None, "no book yet"),
                RiskMultiplier("risk_scale", "Risk scale (stored / target)", None, "no book yet")]
    binding = report.binding_term if report.binding_term != "none" else ""
    m_detail = (f"{_describe(binding)} bound" if binding and report.m < 1.0 - 1e-9
                else "no limit bound: the overlay did not cut the book")
    detail = row.risk_detail if isinstance(row.risk_detail, Mapping) else {}
    m = report.m
    if report.state == MODEL:  # the model's run: m is risk_detail's risk_requested
        m = _optional(detail.get("risk_requested"))
        b = str(detail.get("binding_term") or "none")
        m_detail = (f"{_describe(b)} bound" if m is not None and m < 1.0 - 1e-9 and b != "none"
                    else "no limit bound: the overlay did not cut the book")
    scale = row.risk_scale
    desk = detail.get("desk") if isinstance(detail.get("desk"), Mapping) else {}
    overlay = desk.get("overlay") if isinstance(desk.get("overlay"), Mapping) else {}
    if scale is None:
        scale = _optional(overlay.get("risk_scale"))
    return [
        RiskMultiplier("m", "Risk overlay multiplier (m)", m, m_detail),
        RiskMultiplier("risk_scale", "Risk scale (stored / target)", scale,
                       "the stored book's gross over the capped target's (after rounding, the "
                       "buffer and the cap)"),
    ]


def load_multipliers(config_dir: str, portfolio_dir: str) -> List[RiskMultiplier]:
    """The configured diversification multipliers of each strategy of the portfolio: the IDM
    (`idm`, a fixed setting) and the FDM (the engine's lookup by the number of EMA windows, in the
    strategy's `fdm` else defaults.json `strategy_defaults.fdm`). [] when the files are missing."""
    def read(path: Path):
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None

    portfolio = read(Path(portfolio_dir) / "portfolio.json")
    defaults = read(Path(config_dir) / "defaults.json") or {}
    if not isinstance(portfolio, Mapping):
        return []
    default_fdm = ((defaults.get("strategy_defaults") or {}).get("fdm")
                   if isinstance(defaults, Mapping) else None)
    out = []
    strategies = portfolio.get("strategies")
    for name, strategy in sorted((strategies or {}).items() if isinstance(strategies, Mapping)
                                 else ()):
        if not isinstance(strategy, Mapping) or strategy.get("enabled_live") is False:
            continue
        cfg = strategy.get("config") if isinstance(strategy.get("config"), Mapping) else {}
        idm = _optional(cfg.get("idm"))
        if idm is not None:
            out.append(RiskMultiplier(f"idm:{name}", f"IDM, {name}", idm,
                                      "configured (instrument diversification multiplier; a "
                                      "fixed setting, not estimated per run)"))
        windows = cfg.get("ema_windows")
        if isinstance(windows, list) and windows:
            table = cfg.get("fdm") if isinstance(cfg.get("fdm"), list) else default_fdm
            fdm = 1.0
            for pair in table or ():
                if (isinstance(pair, list) and len(pair) == 2
                        and _optional(pair[0]) == float(len(windows))):
                    fdm = _number(pair[1], 1.0)
                    break
            out.append(RiskMultiplier(f"fdm:{name}", f"FDM, {name}", fdm,
                                      f"configured (forecast diversification multiplier for "
                                      f"{len(windows)} EMA windows)"))
    return out


def over_terms(report: DeskRiskReport) -> List[str]:
    """The terms the stored book is still over: after rounding, then through a held position."""
    out: List[str] = []
    for term in _terms(report.over_limit_after_rounding) + _terms(report.over_limit_by_hold):
        if term not in out:
            out.append(term)
    return out


def fund_cap_check(row: Optional[DeskRiskRow]) -> RiskCheck:
    """Contract C11, the fund's hard cap: the stored book's initial margin against the fund's
    equity after today's transaction costs (100% of equity). The engine refuses a desk or override
    book over it; a model book over it is reported here."""
    label = "Initial margin (fund cap: 100% of equity)"
    if row is None or row.initial_margin is None or row.equity is None:
        return RiskCheck("MARGIN", label, None, None, USD, NOT_MEASURED, "no stored book yet")
    over = row.initial_margin > row.equity + 0.005
    return RiskCheck("MARGIN", label, row.initial_margin, row.equity, USD,
                     OVER if over else WITHIN,
                     ("initial margin exceeds the fund's equity" if over else
                      "the fund's equity after today's transaction costs"))


def publish_blockers(report: DeskRiskReport) -> List[Breach]:
    """What stops the day's book from being published (decision 5): the final book C over a cap
    in BLOCKING_CAPS. [] when it may be published.

    * An incomplete book (no book_source: a desk or override run did not finish) is blocked.
    * The model's book (and no book) is never blocked here: the 10:00 fallback publishes it
      regardless; a margin breach on it is reported by the checks.
    * A desk book is blocked by any blocking cap over its max (read from the value, whatever label
      the check carries), still over after rounding or through a held position, by a blocking cap
      the engine did not measure, and by an overlay that refused (the caps of the held book were
      not checked).
    * An approved override is blocked only by the margin cap.
    """
    if report.state == INCOMPLETE:
        return [Breach(INCOMPLETE, "the day's book is incomplete (a desk or override run did not "
                                   "finish writing it); save again")]
    if report.state not in (DESK, OVERRIDE):
        return []
    waived = OVERRIDABLE_CAPS if report.state == OVERRIDE else ()
    out: List[Breach] = []
    seen = set()

    def add(term: str, detail: str) -> None:
        if term in waived or term in seen:
            return
        seen.add(term)
        out.append(Breach(term, detail))

    by_term = {c.term: c for c in report.checks}
    for term in BLOCKING_CAPS:
        check = by_term.get(term)
        if check is None or check.status == NOT_MEASURED or check.value is None:
            add(term, f"{check.label if check else _describe(term)}: not measured on this book, "
                      "so it cannot be shown to be within its cap")
            continue
        if check.status == OVER or (check.max is not None
                                    and abs(check.value) > check.max + 1e-9):
            add(term, f"{check.label}: {check.value:g} is over its cap of "
                      f"{check.max:g}" if check.max is not None else check.detail)
    for term in over_terms(report):
        if term in BLOCKING_CAPS:
            add(term, f"{_describe(term)}: the stored book is still over its cap")
    if report.refused and report.state == DESK:
        add("refused", "the overlay could not size the book; its caps were not checked")
    return out


# -- the margin cap on a requested book (decision 10, Dom 2026-10-16) ------------------------------
#
# An override asks for B exactly as the desk saved it, and no one may approve a book past the
# fund's margin cap (C11). The engine refuses such a book when the approver decides, but by then
# the day's qt book has been cleared; the request is checked first, from the margin data the
# database holds (metadata.contract_metadata, "Overnight Initial Margin": the figure the engine's
# InstrumentRegistry posts), so a request over the cap is refused before it is e-mailed and a
# decision on one is refused before the engine runs.

# The engine's registry remaps these roots on a continuous (".v.") symbol to the micro contract.
_MICRO = {"ES": "MES", "YM": "MYM", "NQ": "MNQ"}


def contract_root(symbol: str) -> str:
    """The contract_metadata key of a book symbol, as InstrumentRegistry::get_instrument reads it:
    the ".v.N" suffix stripped, and ES/YM/NQ continuous series remapped to the micro contract."""
    root, dot, _ = symbol.partition(".v.")
    return _MICRO.get(root, root) if dot else root


@dataclass(frozen=True)
class MarginInputs:
    """What the cap is measured from: each contract's initial margin per contract (by
    contract_root) and the fund's equity after today's costs (the day's qt row, else the model's)."""
    margins: Mapping[str, float]
    equity: Optional[float]


@dataclass(frozen=True)
class MarginReading:
    initial_margin: Optional[float]
    equity: Optional[float]
    missing: Tuple[str, ...] = ()  # symbols with no margin on record

    @property
    def measured(self) -> bool:
        return self.initial_margin is not None and self.equity is not None and not self.missing

    @property
    def over(self) -> bool:
        return self.measured and self.initial_margin > self.equity + 0.005


def book_margin(positions: Sequence[Tuple[str, float]], inputs: MarginInputs) -> MarginReading:
    """Initial margin of a book given as (symbol, quantity) rows, one per sleeve: the account
    posts margin on each symbol's net quantity (live/book_exposure.hpp), |Σ q| × initial margin."""
    net: dict = {}
    for symbol, qty in positions:
        net[symbol] = net.get(symbol, 0.0) + float(qty)
    total, missing = 0.0, []
    for symbol, qty in sorted(net.items()):
        if abs(qty) < 1e-12:
            continue
        per = inputs.margins.get(contract_root(symbol))
        if per is None or not math.isfinite(per):
            missing.append(symbol)
            continue
        total += abs(qty) * per
    return MarginReading(total, inputs.equity, tuple(missing))


def proposal_positions(payload: Any) -> Optional[List[Tuple[str, float]]]:
    """The (symbol, quantity) rows of an override request's proposal snapshot (contract C1), None
    when the payload carries none."""
    rows = payload.get("proposal") if isinstance(payload, Mapping) else None
    if not isinstance(rows, list):
        return None
    out: List[Tuple[str, float]] = []
    for r in rows:
        if not isinstance(r, Mapping) or not isinstance(r.get("symbol"), str):
            return None
        try:
            out.append((r["symbol"], float(r.get("quantity") or 0.0)))
        except (TypeError, ValueError):
            return None
    return out


def _usd(x: float) -> str:
    return f"${x:,.0f}"


def margin_refusal(reading: MarginReading, date: str) -> Optional[str]:
    """The refusal of a requested book over (or not measurable against) the margin cap, else
    None."""
    if reading.missing:
        return (f"the margin cap (C11) cannot be checked: no initial margin on record for "
                f"{', '.join(reading.missing)} (metadata.contract_metadata); nothing was sent")
    if reading.equity is None:
        return ("the margin cap (C11) cannot be checked: the fund's equity for "
                f"{date} is not stored yet; try again once the day's book is built")
    if reading.over:
        return (f"the {date} book you asked for needs {_usd(reading.initial_margin)} of initial "
                f"margin, more than the fund's equity of {_usd(reading.equity)}: the fund's hard "
                "cap is 100% of equity (contract C11) and no one can approve past it. Save "
                "positions within the cap")
    return None
