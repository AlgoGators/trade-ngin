"""Build the daily-report payload (schema_version 1) from a portfolio's detail.

AlgoLens already assembles a portfolio's detail for its own screens (build_strategy_detail:
positions, executions, the equity curve, the finalized lots and the run metrics), so the report is
a different view of the same numbers: nothing is recomputed here and nothing is read from the
database. The weekend and holiday notices are decided here from the date, as trade-ngin's email
did, so the wording is in one place.
"""

from collections.abc import Mapping, Sequence
from datetime import date, timedelta
from itertools import pairwise
from typing import Any

from .cme_calendar import CLOSURES, GROUP_OF_ROOT
from .daily import SCHEMA_VERSION, money, percent, signed_money

DAILY_CHART_DAYS = 30
COMPOSITION_TOP = 7

FOOTNOTES = (
    "This strategy is based on daily OHLCV data. Only the front-month contract is covered.",
    "All values reflect a trading start date of October 5th, 2025.",
    "As of March 1st, 2026 the tradeable universe also includes ZT (2-Year T-Note), ZF (5-Year "
    "T-Note), 6A (Australian Dollar), 6L (Brazilian Real), HO (Heating Oil) and NG (Natural Gas).",
)


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


def calendar_notices(
    report_date: date, settled: bool = False, roots: Sequence[str] = ()
) -> list[dict[str, str]]:
    """The day-before banners: why yesterday's PnL may be missing, in trade-ngin's wording.

    Yesterday is the settled day. When it was a Saturday, a Sunday or a market holiday and the
    book has no finalized positions (settled=False) the markets were closed and the report says
    nothing was executed; when positions did settle (settled=True) only some contracts lack a
    price, so only the first banner shows. An ordinary weekday gets no banner.
    """
    day = report_date - timedelta(days=1)
    label = day.isoformat()
    weekday = day.weekday()  # Monday is 0
    holiday = holiday_name(day, roots)
    if weekday == 6:
        body = (
            f"Agricultural futures settlement prices for Sunday ({label}) are not yet "
            f"available, as these contracts begin trading Sunday evening. {_SETTLEMENT_PENDING}"
        )
        notices = [_notice("weekend", "Yesterday was Sunday", body, _MONITOR)]
        if not settled:
            notices += [
                _notice(
                    "info",
                    "Note",
                    "Yesterday's PnL data is not available for agricultural contracts.",
                ),
                _notice("alert", "No Executions", _no_session(label)),
            ]
        return notices
    if weekday == 5:
        name, why = "Saturday", "due to it being a Saturday"
    elif holiday:
        name, why = holiday, "due to a federal holiday"
    else:
        return []
    kind = "holiday" if holiday else "weekend"
    if settled:
        body = (
            f"Some futures settlement prices for {label} are not available, as these contracts "
            f"did not trade. {_SETTLEMENT_PENDING}"
        )
        return [_notice(kind, f"Yesterday was {name}", body, _MONITOR)]
    body = (
        f"The latest futures settlement prices are not available, as futures markets were "
        f"closed yesterday ({label}) {why}. {_SETTLEMENT_PENDING}"
    )
    if weekday == 5:
        none = (
            "Futures markets were closed yesterday, so no executions were generated and "
            "portfolio holdings are unchanged. Updates will resume with the next trading "
            "session\u2019s data."
        )
    else:
        none = (
            f"Since yesterday ({label}) was a market holiday, no new market data is available. "
            "Positions remain unchanged from the previous trading day, and no executions were "
            "generated."
        )
    return [
        _notice(kind, f"Yesterday was {name}", body, _MONITOR_CLOSED),
        _notice("info", "Note", "Yesterday's PnL data is not available."),
        _notice("alert", "No Executions", none),
    ]


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


def _executions(detail: Mapping[str, Any]) -> list[dict[str, Any]]:
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
    """trade-ngin's four Performance Metrics groups, in its order. A figure the screens do not
    carry (Sortino, the cumulative cost split) is left out rather than invented."""
    m = detail["metrics"]
    groups: list[list[tuple[str, str, str]]] = [
        [
            ("Unrealized PnL", signed_money(m["unrealizedPnL"], 2), _tone(m["unrealizedPnL"])),
            ("Realized PnL", signed_money(m["realizedPnL"], 2), _tone(m["realizedPnL"])),
            ("Transaction costs", money(m["totalCommissions"], 2), ""),
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


def _previous_day(detail: Mapping[str, Any], report_date: date) -> dict[str, Any]:
    """The settled day's box from what the detail holds: its return, realized and net PnL."""
    curve = detail["historicalData"]
    if len(curve) < 2:
        return {}
    net = float(curve[-1]["value"]) - float(curve[-2]["value"])
    realized = sum(float(r["realizedPnL"]) for r in detail["finalizedPositions"])
    daily_return = detail["metrics"]["dailyReturn"]
    rows = [
        ("Total positions", f"{len(detail['positions'])}", ""),
        ("Daily return", f"{daily_return:.2f}%", _tone(daily_return)),
        ("Daily realized PnL (gross)", signed_money(realized, 2), _tone(realized)),
        ("Daily total PnL (net)", signed_money(net, 2), _tone(net)),
    ]
    return {
        "label": (report_date - timedelta(days=1)).isoformat(),
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
) -> dict[str, Any]:
    """The queue payload for one portfolio's report on report_date."""
    return {
        "schema_version": SCHEMA_VERSION,
        "report_date": report_date.isoformat(),
        "portfolio_id": portfolio_id,
        "portfolio_name": portfolio_name,
        "notices": calendar_notices(
            report_date, settled=bool(detail["finalizedPositions"]), roots=_roots(detail)
        ),
        "positions": _positions(detail),
        "executions": _executions(detail),
        "realized": _realized(detail, report_date),
        "metrics": _metrics(detail),
        "series": {**_series(detail), "drawdown": _drawdown(detail), **dict(history or {})},
        "summary": {"margin_posted": float(detail["metrics"]["marginPosted"])},
        "previous_day": _previous_day(detail, report_date),
        "symbols": dict(symbols or {}),
        "rollovers": [dict(r) for r in rollovers],
        "footnotes": list(FOOTNOTES),
    }


__all__ = ["FOOTNOTES", "build_daily_payload", "calendar_notices", "holiday_name", "percent"]
