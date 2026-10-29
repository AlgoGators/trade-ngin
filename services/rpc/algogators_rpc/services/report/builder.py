"""Build the daily-report payload (schema_version 1) from a portfolio's detail.

The detail (calc.build_strategy_detail) carries the day's stored figures; nothing is recomputed
here and nothing is read from the database. What only the run knows about the day (no session
the day before, a risk module that held the book) comes from the engine as flags (ReportFlags);
the calendar only names a closed day in a banner's title, it never decides that there is one.
"""

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from datetime import date, timedelta
from itertools import pairwise
from typing import Any

from .cme_calendar import CLOSURES, GROUP_OF_ROOT
from .daily import SCHEMA_VERSION, money, signed_money

DAILY_CHART_DAYS = 30
COMPOSITION_TOP = 7

DATA_NOTE = "This strategy is based on daily OHLCV data. Only the front-month contract is covered."
UNIVERSE_NOTE = (
    "As of March 1st, 2026 the tradeable universe also includes ZT (2-Year T-Note), ZF (5-Year "
    "T-Note), 6A (Australian Dollar), 6L (Brazilian Real), HO (Heating Oil) and NG (Natural Gas)."
)


@dataclass(frozen=True)
class ReportFlags:
    """What the engine's run knows about the day and the tables cannot say.

    no_session: the run found no session on the day before and carried its book.
    alert: the run's risk-module (or sizing) failure, in the engine's words; the book was held.
    """

    no_session: bool = False
    alert: str = ""


def long_date(iso: str) -> str:
    """'2025-10-05' -> 'October 5th, 2025'; anything else as it is."""
    try:
        d = date.fromisoformat(iso)
    except (TypeError, ValueError):
        return str(iso)
    n = d.day
    suffix = "th" if 11 <= n % 100 <= 13 else {1: "st", 2: "nd", 3: "rd"}.get(n % 10, "th")
    return f"{d.strftime('%B')} {n}{suffix}, {d.year}"


def footnotes(start_date: str | None) -> list[str]:
    notes = [DATA_NOTE]
    if start_date:
        notes.append(f"All values reflect a trading start date of {long_date(start_date)}.")
    return notes + [UNIVERSE_NOTE]


def holiday_name(day: date, roots: Sequence[str] = ()) -> str | None:
    """The CME closure on day when it closed every product group the book holds, else None."""
    closure = CLOSURES.get(day.isoformat())
    if closure is None:
        return None
    name, closes = closure
    if "all" in closes:
        return name
    groups = {GROUP_OF_ROOT[r] for r in roots if r in GROUP_OF_ROOT}
    return name if groups and groups <= closes else None


_MONITOR = "Please monitor these positions closely."
_MONITOR_CLOSED = "Please continue to monitor your positions closely."
_SETTLEMENT_PENDING = (
    "The PnL for these contracts will be updated in the next report once settlement data is "
    "released."
)


def _notice(kind: str, title: str, body: str, footnote: str = "") -> dict[str, str]:
    return {"kind": kind, "title": title, "body": body, "footnote": footnote}


def day_notices(
    report_date: date, flags: ReportFlags = ReportFlags(), roots: Sequence[str] = ()
) -> list[dict[str, str]]:
    """The banners about the day, from the run's own flags. A day the run found a session for has
    no banner whatever its weekday; the weekday and the CME calendar only name a closed day."""
    notices = []
    if flags.alert:
        notices.append(_notice("alert", "Risk module failed - book held", flags.alert))
    if not flags.no_session:
        return notices
    day = report_date - timedelta(days=1)
    label = day.isoformat()
    holiday = holiday_name(day, roots)
    name = holiday or {5: "Saturday", 6: "Sunday"}.get(day.weekday())
    if name:
        why = "due to a federal holiday" if holiday else f"due to it being a {name}"
        body = (
            f"The latest futures settlement prices are not available, as futures markets were "
            f"closed yesterday ({label}) {why}. {_SETTLEMENT_PENDING}"
        )
        kind = "holiday" if holiday else "weekend"
        notices.append(_notice(kind, f"Yesterday was {name}", body, _MONITOR_CLOSED))
    notices += [
        _notice("alert", "No Executions", _no_session(label)),
        _notice("info", "Note", "Yesterday's PnL data is not available."),
    ]
    return notices


def _no_session(label: str) -> str:
    return (
        f"No session was found for yesterday ({label}), so no new market data is available. "
        "Positions remain unchanged from the previous trading day, and no executions were "
        "generated."
    )


def _symbol(name: str) -> str:
    return name.replace(".v.0", "")


def _roots(detail: Mapping[str, Any]) -> list[str]:
    held = [*detail["positions"], *detail["finalizedPositions"]]
    return sorted({_symbol(p["symbol"]) for p in held})


def _signed(quantity: float, notional: float) -> float:
    return -abs(notional) if quantity < 0 else abs(notional)


def _positions(detail: Mapping[str, Any]) -> list[dict[str, Any]]:
    out = []
    for p in detail["positions"]:
        quantity = float(p["quantity"])
        out.append(
            {
                "symbol": _symbol(p["symbol"]),
                "name": "",
                "quantity": quantity,
                "price": float(p["marketPrice"]),
                "notional": _signed(quantity, float(p["notional"])),
                "weight": float(p["percentOfTotal"]) / 100.0,
            }
        )
    return out


def _executions(detail: Mapping[str, Any], roll: bool = False) -> list[dict[str, Any]]:
    """The day's strategy fills, or (roll=True) its roll legs; a roll leg is never a trade."""
    return [
        {
            "symbol": _symbol(e["symbol"]),
            "side": str(e["side"]),
            "quantity": float(e["quantity"]),
            "price": float(e["price"]),
            "notional": float(e["notional"]),
            "cost": float(e["commission"]),
        }
        for e in detail["executions"]
        if (e.get("type", "STRATEGY") == "ROLL") == roll
    ]


def _realized(detail: Mapping[str, Any], report_date: date) -> dict[str, Any]:
    rows = detail["finalizedPositions"]
    if not rows:
        return {}
    return {
        "label": (report_date - timedelta(days=1)).isoformat(),
        "positions": [
            {
                "symbol": r["symbol"],
                "quantity": float(r["quantity"]),
                "entry_price": float(r["entryPrice"]),
                "exit_price": float(r["exitPrice"]),
                "pnl": float(r["realizedPnL"]),
            }
            for r in rows
        ],
    }


def _day(label: str) -> str:
    return label[:10]


def _series(detail: Mapping[str, Any]) -> dict[str, list[dict[str, Any]]]:
    curve: list[dict[str, Any]] = [
        {"date": _day(pt["date"]), "value": float(pt["value"])} for pt in detail["historicalData"]
    ]
    equity = curve[-(DAILY_CHART_DAYS + 1) :]
    daily = [
        {"date": cur["date"], "value": round(cur["value"] - prev["value"], 2)}
        for prev, cur in pairwise(equity)
    ]
    by_symbol = [
        {"symbol": r["symbol"], "value": float(r["realizedPnL"])}
        for r in detail["finalizedPositions"]
    ]
    weights = sorted(
        ((_symbol(p["symbol"]), abs(float(p["notional"]))) for p in detail["positions"]),
        key=lambda kv: -kv[1],
    )
    head, tail = weights[:COMPOSITION_TOP], weights[COMPOSITION_TOP:]
    composition = [{"label": s, "value": v} for s, v in head if v > 0]
    if tail and sum(v for _, v in tail) > 0:
        composition.append({"label": "Other", "value": sum(v for _, v in tail)})
    return {
        "equity": equity,
        "daily_pnl": daily,
        "pnl_by_symbol": by_symbol,
        "composition": composition,
    }


def _tone(value: float) -> str:
    if abs(value) < 1e-9:
        return ""
    return "up" if value > 0 else "down"


def _metrics(detail: Mapping[str, Any]) -> list[dict[str, Any]]:
    """The four Performance Metrics groups, in the engine's order; every figure is the stored one."""
    m = detail["metrics"]
    groups: list[list[tuple[str, str, str]]] = [
        [
            ("Unrealized PnL", signed_money(m["unrealizedPnL"], 2), _tone(m["unrealizedPnL"])),
            ("Realized PnL", signed_money(m["realizedPnL"], 2), _tone(m["realizedPnL"])),
            ("Transaction costs", money(m["totalCommissions"], 2), ""),
            *(
                [("Roll costs", money(m["totalRollCosts"], 2), "")]
                if m.get("totalRollCosts") is not None
                else []
            ),
            ("Total PnL (net)", signed_money(m["netPnL"], 2), _tone(m["netPnL"])),
            ("Current portfolio value", money(m["currentPortfolioValue"], 2), ""),
        ],
        [
            (
                "Total cumulative return",
                f"{m['cumulativeReturn']:.2f}%",
                _tone(m["cumulativeReturn"]),
            ),
            ("Annualized return", f"{m['annualizedReturn']:.2f}%", _tone(m["annualizedReturn"])),
            ("Sharpe ratio", f"{m['sharpeRatio']:.2f}", ""),
            ("Sortino ratio", f"{m['sortinoRatio']:.2f}", ""),
            ("Volatility", f"{m['volatility']:.2f}%", ""),
            ("Max drawdown", f"{m['maxDrawdown']:.2f}%", ""),
        ],
        [
            ("Cash available", money(m["cashAvailable"], 2), ""),
            ("Margin posted", money(m["marginPosted"], 2), ""),
            ("Equity-to-margin ratio", f"{m['equityToMarginRatio']:.2f}x", ""),
            ("Margin cushion", f"{m['marginCushion']:.2f}%", ""),
        ],
        [
            ("Gross leverage", f"{m['grossLeverage']:.2f}x", ""),
            ("Net leverage", f"{m['netLeverage']:.2f}x", ""),
        ],
    ]
    return [
        {"label": label, "value": value, "group": g, "tone": tone}
        for g, rows in enumerate(groups)
        for label, value, tone in rows
    ]


def _previous_day(detail: Mapping[str, Any]) -> dict[str, Any]:
    """The settled day's box: its stored daily figures (the previous live_results row)."""
    prev = detail.get("previousDay")
    if not prev:
        return {}
    rows = [("Total positions", f"{len(detail['finalizedPositions'])}", "")]
    for label, key, kind in (
        ("Daily return", "dailyReturn", "pct"),
        ("Daily unrealized PnL (gross)", "dailyUnrealizedPnL", "pnl"),
        ("Daily realized PnL (gross)", "dailyRealizedPnL", "pnl"),
        ("Daily transaction costs", "dailyTransactionCosts", "cost"),
        ("Daily roll costs", "dailyRollCosts", "cost"),
        ("Daily total PnL (net)", "dailyPnL", "pnl"),
    ):
        value = prev.get(key)
        if value is None:
            continue
        if kind == "pct":
            rows.append((label, f"{value:.2f}%", _tone(value)))
        elif kind == "cost":
            rows.append((label, money(abs(value), 2), ""))
        else:
            rows.append((label, signed_money(value, 2), _tone(value)))
    return {
        "label": prev["date"],
        "metrics": [
            {"label": label, "value": value, "group": 0, "tone": tone}
            for label, value, tone in rows
        ],
    }


def _drawdown(detail: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Percent below the running peak of the whole curve, for its last DAILY_CHART_DAYS points."""
    peak = 0.0
    out = []
    for pt in detail["historicalData"]:
        value = float(pt["value"])
        peak = max(peak, value)
        drop = round((value / peak - 1) * 100, 4) if peak else 0.0
        out.append({"date": _day(pt["date"]), "value": drop})
    return out[-DAILY_CHART_DAYS:]


def build_daily_payload(
    detail: Mapping[str, Any],
    portfolio_id: str,
    portfolio_name: str,
    report_date: date,
    rollovers: Sequence[Mapping[str, str]] = (),
    symbols: Mapping[str, Any] | None = None,
    history: Mapping[str, Any] | None = None,
    flags: ReportFlags = ReportFlags(),
) -> dict[str, Any]:
    """The queue payload for one portfolio's report on report_date."""
    return {
        "schema_version": SCHEMA_VERSION,
        "report_date": report_date.isoformat(),
        "portfolio_id": portfolio_id,
        "portfolio_name": portfolio_name,
        "notices": day_notices(report_date, flags, roots=_roots(detail)),
        "positions": _positions(detail),
        "executions": _executions(detail),
        "roll_legs": _executions(detail, roll=True),
        # a carried day has no settled table: the run found no prices for it
        "realized": {} if flags.no_session else _realized(detail, report_date),
        "metrics": _metrics(detail),
        "series": {**_series(detail), "drawdown": _drawdown(detail), **dict(history or {})},
        "summary": {"margin_posted": float(detail["metrics"]["marginPosted"])},
        "previous_day": _previous_day(detail),
        "symbols": dict(symbols or {}),
        "rollovers": [dict(r) for r in rollovers],
        "footnotes": footnotes(detail.get("startDate")),
    }


__all__ = ["ReportFlags", "build_daily_payload", "day_notices", "footnotes", "holiday_name"]
