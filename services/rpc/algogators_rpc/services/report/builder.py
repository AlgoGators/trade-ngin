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
    equities: bool = False  # an equity book: no futures notes, margin lines or symbols reference
    rolls: Mapping[str, Any] | None = None  # the engine's roll texts (report_rolls_json.hpp)


def long_date(iso: str) -> str:
    """'2025-10-05' -> 'October 5th, 2025'; anything else as it is."""
    try:
        d = date.fromisoformat(iso)
    except (TypeError, ValueError):
        return str(iso)
    n = d.day
    suffix = "th" if 11 <= n % 100 <= 13 else {1: "st", 2: "nd", 3: "rd"}.get(n % 10, "th")
    return f"{d.strftime('%B')} {n}{suffix}, {d.year}"


EQUITY_NOTE = "This strategy is based on daily OHLCV data for equity assets."


def footnotes(start_date: str | None, flags: "ReportFlags | None" = None) -> list[str]:
    """The footer's notes. With per_contract_rolls on the futures sentence is the engine's
    (kReportRollsFooter); an equity book has its own and no futures universe note."""
    flags = flags or ReportFlags()
    if flags.equities:
        notes = [EQUITY_NOTE]
    elif flags.rolls and flags.rolls.get("footer"):
        notes = ["This strategy is based on daily OHLCV data. " + str(flags.rolls["footer"])]
    else:
        notes = [DATA_NOTE]
    if start_date:
        notes.append(f"All values reflect a trading start date of {long_date(start_date)}.")
    return notes if flags.equities else notes + [UNIVERSE_NOTE]


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
    report_date: date,
    flags: ReportFlags = ReportFlags(),
    roots: Sequence[str] = (),
    some_missing: bool = False,
) -> list[dict[str, str]]:
    """The banners about the day before (the C++ e-mail's day_before_banner, word for word).

    The data picks the form: no session at all (the run's --no-session) is the closed form; a
    held row with no price or a rejected one (`some_missing`, from the stored settle marks) is
    the partial form. The weekday and the CME calendar only pick the title; a day with nothing
    missing has no banner whatever its weekday. A held book's alert comes first."""
    notices = []
    if flags.alert:
        notices.append(_notice("alert", "Risk module failed - book held", flags.alert))
    if not flags.no_session and not some_missing:
        return notices
    day = report_date - timedelta(days=1)
    label = day.isoformat()
    holiday = holiday_name(day, roots)
    saturday = not holiday and day.weekday() == 5
    sunday = not holiday and day.weekday() == 6
    sunday_body = (
        f"Agricultural futures settlement prices for Sunday ({label}) are not yet available, as "
        f"these contracts begin trading Sunday evening. {_SETTLEMENT_PENDING}"
    )
    if flags.no_session:
        note = "Yesterday's PnL data is not available."
        if holiday or saturday:
            name = holiday or "Saturday"
            why = "due to a federal holiday" if holiday else "due to it being a Saturday"
            notices.append(
                _notice(
                    "holiday" if holiday else "weekend",
                    f"Yesterday was {name}",
                    f"The latest futures settlement prices are not available, as futures markets "
                    f"were closed yesterday ({label}) {why}. {_SETTLEMENT_PENDING}",
                    _MONITOR_CLOSED,
                )
            )
            red = (
                f"Since yesterday ({label}) was a market holiday, no new market data is "
                "available. Positions remain unchanged from the previous trading day, and no "
                "executions were generated."
                if holiday
                else "Futures markets were closed yesterday, so no executions were generated "
                "and portfolio holdings are unchanged. Updates will resume with the next "
                "trading session\u2019s data."
            )
        else:
            if sunday:
                notices.append(_notice("weekend", "Yesterday was Sunday", sunday_body, _MONITOR))
                note = "Yesterday's PnL data is not available for agricultural contracts."
            red = _no_session(label)
        notices += [_notice("alert", "No Executions", red), _notice("info", "Note", note)]
        return notices
    partial = (
        f"Some futures settlement prices for {label} are not available, as these contracts did "
        f"not trade. {_SETTLEMENT_PENDING}"
    )
    if holiday:
        notices.append(_notice("holiday", f"Yesterday was {holiday}", partial, _MONITOR))
    elif saturday:
        notices.append(_notice("weekend", "Yesterday was Saturday", partial, _MONITOR))
    elif sunday:
        notices.append(_notice("weekend", "Yesterday was Sunday", sunday_body, _MONITOR))
    # a weekday with a few rows missing: the settled table's footnote 1 only
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


# The footnotes of the rows (the C++ e-mail's kinds): numbered in the order they first appear.
NOTE_TEXT = {
    1: "No price received that day. PnL is logged as 0 and updates on later days.",
    2: "Contract rolled that day. PnL is not booked on the roll.",
    3: "Price rejected: too little trading volume that day. PnL is booked with the next "
    "accepted price.",
    4: "No price bar for this contract that day. Its last known price is shown.",
}
ROLL_BAR_NOTE = 5  # its text is the engine's (report_rolls_json: roll_bar_note)
_MARK_NOTE = {"NO_BAR": 1, "ROLL": 2, "WITHHELD": 3}


def sleeve_name(strategy_name: str) -> str:
    """'TREND_FOLLOWING_FAST' -> 'Trend Following Fast'."""
    return " ".join(w.capitalize() for w in str(strategy_name or "").split("_") if w)


def _positions(detail: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Today's rows per sleeve; the weight is the row's share of its sleeve's notional."""
    sleeve_total: dict[str, float] = {}
    for p in detail["positions"]:
        sleeve_total[p["sleeve"]] = sleeve_total.get(p["sleeve"], 0.0) + abs(float(p["notional"]))
    out = []
    for p in detail["positions"]:
        quantity = float(p["quantity"])
        total = sleeve_total.get(p["sleeve"], 0.0)
        out.append(
            {
                "symbol": _symbol(p["symbol"]),
                "name": "",
                "quantity": quantity,
                "price": float(p["marketPrice"]),
                "notional": _signed(quantity, float(p["notional"])),
                "weight": abs(float(p["notional"])) / total if total > 0 else 0.0,
                "sleeve": sleeve_name(p["sleeve"]),
                "note": 4 if p.get("noBar") else 0,
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
            "net_cost": float(e.get("netCost", e["commission"])),
            "sleeve": sleeve_name(e.get("sleeve", "")),
        }
        for e in detail["executions"]
        # a BORROW row (the backtest's overnight fee, quantity 0) is neither a trade nor a leg
        if e.get("type", "STRATEGY") == ("ROLL" if roll else "STRATEGY")
    ]


def netting_note(detail: Mapping[str, Any]) -> str:
    """The day's netting between sleeves, as the C++ e-mail stated it; empty when nothing netted."""
    rows = detail["executions"]
    if not any(abs(float(e.get("nettingAdjustment", 0.0))) > 1e-12 for e in rows):
        return ""
    own = sum(float(e["commission"]) for e in rows)
    paid = sum(float(e.get("netCost", e["commission"])) for e in rows)
    return (
        f"Netting between strategies: own costs {money(own, 2)}, adjustment "
        f"{signed_money(paid - own, 2)}, transaction costs {money(paid, 2)}."
    )


def _realized(detail: Mapping[str, Any], report_date: date) -> dict[str, Any]:
    rows = detail["finalizedPositions"]
    if not rows:
        return {}
    prev = detail.get("previousDay") or {}
    return {
        "label": prev.get("date") or (report_date - timedelta(days=1)).isoformat(),
        "positions": [
            {
                "symbol": r["symbol"],
                "quantity": float(r["quantity"]),
                "entry_price": r["entryPrice"],
                "exit_price": r["exitPrice"],
                "pnl": float(r["realizedPnL"]),
                "sleeve": sleeve_name(r.get("sleeve", "")),
                "note": _MARK_NOTE.get(r.get("mark", ""), 0),
            }
            for r in rows
        ],
    }


def mark_roll_bars(payload: dict[str, Any], rolls: Mapping[str, Any] | None) -> None:
    """With per_contract_rolls on, a settled row of a symbol that rolled on the settled day's bar
    carries the engine's roll note (it replaces note 2: the old contract's move IS booked)."""
    symbols = {str(s).replace(".v.0", "") for s in (rolls or {}).get("roll_bar_symbols", [])}
    for row in (payload["realized"] or {}).get("positions", []):
        if row["symbol"] in symbols:
            row["note"] = ROLL_BAR_NOTE


def number_notes(payload: dict[str, Any], rolls: Mapping[str, Any] | None = None) -> None:
    """Turn the rows' note kinds into numbers in the order the notes first appear (positions,
    then the settled table), and list the notes under `notes`."""
    numbers: dict[int, int] = {}
    rows = [*payload["positions"], *(payload["realized"] or {}).get("positions", [])]
    for row in rows:
        kind = row.get("note", 0)
        if kind:
            row["note"] = numbers.setdefault(kind, len(numbers) + 1)
    texts = {**NOTE_TEXT, ROLL_BAR_NOTE: str((rolls or {}).get("roll_bar_note") or NOTE_TEXT[2])}
    payload["notes"] = [{"number": n, "text": texts[k]} for k, n in numbers.items()]


def some_settle_price_missing(detail: Mapping[str, Any]) -> bool:
    """A held row of the settled table has no price that day or a rejected one."""
    return any(r.get("mark") in ("NO_BAR", "WITHHELD") for r in detail["finalizedPositions"])


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


def annualized_label(m: Mapping[str, Any]) -> str:
    """The annualized return's label: an extrapolation while the statistics are taken on fewer
    sessions than a year of the grid (n < K, both stored on the row)."""
    n, k = m.get("statisticsSessions"), m.get("sessionsPerYear")
    if n and k and n < k:
        return "Annualized return (extrapolated: under one year of sessions)"
    return "Annualized return"


def _symbols_with_rolls(symbols, rolls):
    """The symbols reference; with per_contract_rolls on, each row's Front Month (the contract
    held) and its cycle's months, as the engine states them."""
    out = dict(symbols or {})
    if not rolls:
        return out
    front, months = rolls.get("front_month", {}), rolls.get("contract_months", {})
    out["rows"] = [
        {**r, "front_month": front.get(r["db"], ""), "months": months.get(r["db"], r["months"])}
        for r in out.get("rows", [])
    ]
    return out


def _sleeve_summary(detail: Mapping[str, Any]) -> list[dict[str, Any]]:
    """The book under the per-sleeve tables: its rows, and the account's stored notional and margin
    (which net sleeves holding one symbol on opposite sides)."""
    m = detail["metrics"]
    rows = [
        ("Sleeve positions", f"{len(detail['positions'])}"),
        ("Total notional", money(m["totalNotional"], 2) if m.get("totalNotional") is not None else None),
        ("Total margin posted", money(m["marginPosted"], 2) if m.get("marginPosted") is not None else None),
    ]
    rows = [(label, value) for label, value in rows if value is not None]
    return [{"label": label, "value": value, "group": 0, "tone": ""} for label, value in rows]


def _metrics(detail: Mapping[str, Any], equities: bool = False) -> list[dict[str, Any]]:
    """The four Performance Metrics groups, in the engine's order; every figure is the stored one.
    A figure stored NULL has no line (the C++ e-mail named a line only when it had a value)."""
    m = detail["metrics"]

    def line(label, key, kind):
        value = m.get(key)
        if value is None:
            return None
        if kind == "pnl":
            return (label, signed_money(value, 2), _tone(value))
        if kind == "money":
            return (label, money(value, 2), "")
        if kind == "ret":
            return (label, f"{value:.2f}%", _tone(value))
        if kind == "pct":
            return (label, f"{value:.2f}%", "")
        if kind == "x":
            return (label, f"{value:.2f}x", "")
        return (label, f"{value:.2f}", "")

    sizing = (
        "Sizing capital",
        money(m["sizingCapital"], 2)
        if m.get("sizingCapital") is not None
        else "no reading (no sized rebalance this run)",
        "",
    )
    groups = [
        [
            line("Unrealized PnL", "unrealizedPnL", "pnl"),
            line("Realized PnL", "realizedPnL", "pnl"),
            line("Transaction costs", "totalCommissions", "money"),
            None if equities else line("Roll costs", "totalRollCosts", "money"),
            line("Total PnL (net)", "netPnL", "pnl"),
            line("Current portfolio value", "currentPortfolioValue", "money"),
        ],
        [
            line("Total cumulative return", "cumulativeReturn", "ret"),
            line(annualized_label(m), "annualizedReturn", "ret"),
            line("Sharpe ratio", "sharpeRatio", "num"),
            line("Sortino ratio", "sortinoRatio", "num"),
            line("Volatility", "volatility", "pct"),
            line("Max drawdown", "maxDrawdown", "pct"),
        ],
        [
            line("Cash available", "cashAvailable", "money"),
            line("Margin posted", "marginPosted", "money"),
            None if equities else sizing,
            # meaningless on a cash account (HD 2026-10-14, section 16.7)
            None if equities else line("Equity-to-margin ratio", "equityToMarginRatio", "x"),
            None if equities else line("Margin cushion", "marginCushion", "pct"),
        ],
        [
            line("Gross leverage", "grossLeverage", "x"),
            line("Net leverage", "netLeverage", "x"),
        ],
    ]
    return [
        {"label": row[0], "value": row[1], "group": g, "tone": row[2]}
        for g, rows in enumerate(groups)
        for row in rows
        if row is not None
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
    symbols: Mapping[str, Any] | None = None,
    history: Mapping[str, Any] | None = None,
    flags: ReportFlags = ReportFlags(),
) -> dict[str, Any]:
    """The queue payload for one portfolio's report on report_date."""
    payload = {
        "schema_version": SCHEMA_VERSION,
        "report_date": report_date.isoformat(),
        "portfolio_id": portfolio_id,
        "portfolio_name": portfolio_name,
        "notices": day_notices(
            report_date, flags, roots=_roots(detail),
            some_missing=not flags.no_session and some_settle_price_missing(detail),
        ),
        "positions": _positions(detail),
        "executions": _executions(detail),
        "roll_legs": _executions(detail, roll=True),
        # a carried day has no settled table: the run found no prices for it
        "realized": {} if flags.no_session else _realized(detail, report_date),
        "metrics": _metrics(detail, flags.equities),
        "series": {**_series(detail), "drawdown": _drawdown(detail), **dict(history or {})},
        "summary": {"margin_posted": float(detail["metrics"]["marginPosted"])},
        "previous_day": _previous_day(detail),
        "symbols": {} if flags.equities else _symbols_with_rolls(symbols, flags.rolls),
        "footnotes": footnotes(detail.get("startDate"), flags),
        "roll_lines": list((flags.rolls or {}).get("lines", [])),
        "rollover": [dict(r) for r in (flags.rolls or {}).get("rollover", [])],
        "netting_note": netting_note(detail),
        "sleeve_summary": _sleeve_summary(detail),
    }
    mark_roll_bars(payload, flags.rolls)
    number_notes(payload, flags.rolls)
    return payload


__all__ = ["ReportFlags", "build_daily_payload", "day_notices", "footnotes", "holiday_name",
           "sleeve_name"]
