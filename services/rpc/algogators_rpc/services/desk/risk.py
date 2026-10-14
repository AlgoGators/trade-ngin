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


@dataclass(frozen=True)
class DeskRiskRow:
    """The day's qt row of trading.live_results, as the store reads it."""

    book_source: Optional[str] = None
    risk_detail: Optional[Mapping[str, Any]] = None
    gross_leverage: Optional[float] = None
    net_leverage: Optional[float] = None
    margin_cushion: Optional[float] = None
    equity_to_margin: Optional[float] = None


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
                     limits: Sequence[Tuple[str, float]] = ()) -> DeskRiskReport:
    """The risk report of a day's qt book. `row` is None when the day has no qt row yet."""
    if row is None:
        return DeskRiskReport(state=NO_BOOK, limits=tuple(limits))
    readings = dict(gross_leverage=row.gross_leverage, net_leverage=row.net_leverage,
                    margin_cushion=row.margin_cushion, equity_to_margin=row.equity_to_margin,
                    limits=tuple(limits))
    detail = row.risk_detail if isinstance(row.risk_detail, Mapping) else {}
    desk = detail.get("desk") if isinstance(detail.get("desk"), Mapping) else None
    source = (row.book_source or "").strip().lower()
    if desk is None or source not in (DESK, OVERRIDE):
        # The model's book: the engine's own run, inside its limits by construction.
        return DeskRiskReport(state=MODEL, **readings)

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
    return DeskRiskReport(
        state=OVERRIDE if approved_override else DESK,
        audit_id=int(_number(desk.get("audit_id"))),
        within_limits=not breaches,
        approved_override=approved_override,
        breaches=tuple(breaches),
        m=m, binding_term=binding, over_limit_after_rounding=after_rounding,
        over_limit_by_hold=by_hold, refused=refused, symbols=tuple(symbols), **readings)


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
