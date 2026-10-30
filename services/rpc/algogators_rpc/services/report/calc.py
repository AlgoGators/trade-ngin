"""Portfolio detail from stored rows: the pure transforms the daily report is built on.

Ported from AlgoLens (research-api domain/portfolio/calculations.py, research-core returns.py and
build_strategy_detail), which this module replaces as the one place the report's numbers are made.
Stdlib only.
"""

from __future__ import annotations

import json
from collections.abc import Mapping, Sequence
from typing import Any


def _get(row: Any, key: str, default: Any = None) -> Any:
    if isinstance(row, Mapping):
        return row.get(key, default)
    return getattr(row, key, default)


def float_or_default(value: Any, default: float = 0) -> float:
    return float(value) if value is not None else default


def parse_contract_size(raw: Any) -> float | None:
    """metadata.contract_metadata."Contract Size" is text ('1,000', '$50'); positive or None."""
    try:
        size = float(str(raw).replace(",", "").replace("$", "").strip())
    except (TypeError, ValueError):
        return None
    return size if size > 0 else None


def resolve_initial_equity(equity_curve: Sequence[Any], base_equity: float) -> float:
    """The true first equity point, else the configured base equity."""
    if equity_curve:
        return float(_get(equity_curve[0], "equity"))
    return base_equity


def build_historical_data(equity_curve: Sequence[Any]) -> list[dict[str, Any]]:
    return [
        {"date": _get(point, "timestamp").isoformat(), "value": float(_get(point, "equity"))}
        for point in equity_curve
    ]


def contract_size(symbol: str | None, sizes: Mapping[str, float] | None) -> float:
    """The contract multiplier of a symbol ('ZF.v.0' -> sizes['ZF']); 1 when unknown."""
    root = (symbol or "").split(".", 1)[0]
    return float((sizes or {}).get(root) or 1.0)


def _opt(value: Any) -> float | None:
    return None if value is None else float(value)


def _marks_written(rows: Sequence[Any], *keys: str) -> bool:
    """Whether the rows were written by a binary of migration 041: one of them carries a value in
    one of the columns. Rows written before carry none, and their NULLs say nothing."""
    return any(_get(r, k) is not None for r in rows for k in keys)


def transform_positions(
    positions: Sequence[Any], current_value: float, sizes: Mapping[str, float] | None = None
) -> list[dict[str, Any]]:
    """Today's book, one row per sleeve and symbol. A row is valued at its mark (mark_price, the
    T-1 close); a row with no mark printed no bar and is valued at its last known price
    (average_price), which the e-mail notes (note 4). Rows written before migration 041 have no
    mark: they are valued at average_price, with no note."""
    marked = _marks_written(positions, "mark_price")
    out = []
    for pos in positions:
        quantity = float(_get(pos, "quantity"))
        mark = _opt(_get(pos, "mark_price"))
        price = mark if mark is not None else float(_get(pos, "average_price"))
        notional = abs(quantity * price * contract_size(_get(pos, "symbol"), sizes))
        out.append(
            {
                "symbol": _get(pos, "symbol"),
                "sleeve": _get(pos, "strategy_name") or "",
                "quantity": quantity,
                "marketPrice": price,
                "noBar": marked and mark is None,
                "notional": notional,
                "percentOfTotal": (notional / current_value * 100) if current_value > 0 else 0,
            }
        )
    return out


def transform_executions(
    executions: Sequence[Any], sizes: Mapping[str, float] | None = None
) -> list[dict[str, Any]]:
    """The day's fills, one row per sleeve and fill. `commission` is the row's own cost (the
    total transaction cost of its fill); `netCost` is what the account paid after netting
    between sleeves (013): the own cost less the netting adjustment."""
    out = []
    for execution in executions:
        exec_time = _get(execution, "execution_time")
        quantity = float(_get(execution, "quantity"))
        price = float(_get(execution, "price"))
        size = contract_size(_get(execution, "symbol"), sizes)
        own = _get(execution, "total_transaction_costs")
        own = float(own if own is not None else (_get(execution, "commissions_fees") or 0))
        adjustment = float(_get(execution, "netting_adjustment") or 0)
        out.append(
            {
                "symbol": _get(execution, "symbol"),
                "sleeve": _get(execution, "strategy_name") or "",
                "side": _get(execution, "side"),
                "quantity": quantity,
                "price": price,
                "notional": quantity * price * size,
                "commission": own,
                "netCost": own - adjustment,
                "nettingAdjustment": adjustment,
                "type": str(_get(execution, "execution_type") or "STRATEGY").upper(),
                "date": exec_time.isoformat() if exec_time else None,
            }
        )
    return out


def transform_finalized(
    yesterday_positions: Sequence[Any], positions: Sequence[Any]
) -> list[dict[str, Any]]:
    """The settled day's book, every held row of every sleeve, with the settle's P&L as stored on
    the row (daily_realized_pnl, written when the next run settles the day).

    The two prices are the ones the settle used (settle_from_price, settle_to_price) and the mark
    says why a row booked no move (settle_mark: NO_BAR, ROLL, WITHHELD). On rows settled before
    migration 041 they are the row's mark on the settled day and today's mark of the same row."""
    settled = _marks_written(yesterday_positions, "settle_from_price", "settle_to_price",
                             "settle_mark")
    today = {(_get(p, "strategy_name") or "", _get(p, "symbol")): p for p in positions}
    out = []
    for yesterday in yesterday_positions:
        quantity = float(_get(yesterday, "quantity"))
        if quantity == 0:
            continue
        symbol = _get(yesterday, "symbol")
        sleeve = _get(yesterday, "strategy_name") or ""
        if settled:
            entry = _opt(_get(yesterday, "settle_from_price"))
            exit_ = _opt(_get(yesterday, "settle_to_price"))
            mark = _get(yesterday, "settle_mark") or ""
        else:
            entry = float(_get(yesterday, "average_price"))
            today_pos = today.get((sleeve, symbol))
            exit_ = float(_get(today_pos, "average_price")) if today_pos else entry
            mark = ""
        out.append(
            {
                "symbol": symbol.replace(".v.0", ""),
                "sleeve": sleeve,
                "quantity": quantity,
                "entryPrice": entry,
                "exitPrice": exit_,
                "mark": mark,
                "realizedPnL": float(_get(yesterday, "daily_realized_pnl") or 0),
            }
        )
    return sorted(out, key=lambda r: (r["sleeve"], r["symbol"]))


def _sizing_capital(row: Mapping[str, Any] | None) -> float | None:
    """risk_detail.sizing_capital of the row: stored only on a day with a sized rebalance."""
    detail = (row.get("risk_detail") if isinstance(row, Mapping) else None) or {}
    if isinstance(detail, str):
        try:
            detail = json.loads(detail)
        except ValueError:
            return None
    value = detail.get("sizing_capital") if isinstance(detail, Mapping) else None
    try:
        return None if value is None else float(value)
    except (TypeError, ValueError):
        return None


def _stored(row: Mapping[str, Any] | None, key: str) -> float | None:
    """A stored figure of a live_results row, or None when the row or the column has none."""
    if not row:
        return None
    value = row.get(key) if isinstance(row, Mapping) else getattr(row, key, None)
    return None if value is None else float(value)


def build_strategy_detail(cfg: Mapping[str, Any], rows: Mapping[str, Any]) -> dict[str, Any] | None:
    """The report's view of one portfolio's day. `rows` holds latest (the day's live_results row),
    previous (the settled day's row), equity_curve, positions, executions, yesterday_positions,
    contract_sizes, book and start. Every figure is the stored one; None: no row for the day."""
    latest = rows.get("latest")
    if not latest:
        return None

    equity_curve = rows.get("equity_curve") or []
    initial_equity = resolve_initial_equity(equity_curve, float(cfg["initial_equity"]))
    if latest.get("current_portfolio_value") is None:
        return None  # a row with no account value is not a day the report can show
    current_value = float(latest["current_portfolio_value"])
    previous = rows.get("previous")

    historical_data = build_historical_data(equity_curve)
    sizes = rows.get("contract_sizes")
    positions = rows.get("positions") or []
    transformed_executions = transform_executions(rows.get("executions") or [], sizes)
    start = rows.get("start")

    return {
        "id": cfg["id"],
        "name": cfg["name"],
        "description": cfg.get("description"),
        "invested": initial_equity,
        "currentValue": current_value,
        "return": _stored(latest, "total_pnl"),
        "returnPercent": _stored(latest, "total_cumulative_return"),
        "positions": transform_positions(positions, current_value, sizes),
        "historicalData": historical_data,
        "executions": transformed_executions,
        "finalizedPositions": transform_finalized(rows.get("yesterday_positions") or [], positions),
        "lastUpdate": latest["date"].isoformat(),
        "book": rows.get("book"),
        "startDate": start.isoformat() if hasattr(start, "isoformat") else (start or None),
        # the settled day's stored figures (the previous row, settled by today's run)
        "previousDay": None
        if not previous
        else {
            "date": previous["date"].isoformat(),
            "dailyReturn": _stored(previous, "daily_return"),
            "dailyUnrealizedPnL": _stored(previous, "daily_unrealized_pnl"),
            "dailyRealizedPnL": _stored(previous, "daily_realized_pnl"),
            "dailyTransactionCosts": _stored(previous, "daily_transaction_costs"),
            "dailyRollCosts": _stored(previous, "daily_roll_costs"),
            "dailyPnL": _stored(previous, "daily_pnl"),
        },
        "metrics": {
            "volatility": _stored(latest, "volatility"),
            "sharpeRatio": _stored(latest, "sharpe_ratio"),
            "sortinoRatio": _stored(latest, "sortino_ratio"),
            "maxDrawdown": _stored(latest, "max_drawdown"),
                        "totalTrades": sum(1 for e in transformed_executions if e["type"] == "STRATEGY"),
            "cumulativeReturn": _stored(latest, "total_cumulative_return"),
            "annualizedReturn": _stored(latest, "total_annualized_return"),
            "totalRollCosts": _stored(latest, "total_roll_costs"),
            # K of the statistics grid and n, the sessions the statistics are taken on (041)
            "sessionsPerYear": _stored(latest, "statistics_sessions_per_year"),
            "statisticsSessions": _stored(latest, "total_days"),
            # the sizing capital the day was sized on (risk_detail, 020); None: no reading
            "sizingCapital": _sizing_capital(latest),
            # the futures runners write gross leverage to portfolio_leverage and leave
            # gross_leverage NULL
            "grossLeverage": _stored(latest, "gross_leverage")
            if _get(latest, "gross_leverage") is not None
            else _stored(latest, "portfolio_leverage"),
            "netLeverage": _stored(latest, "net_leverage"),
            "portfolioLeverage": _stored(latest, "portfolio_leverage"),
            "marginPosted": _stored(latest, "margin_posted"),
            "equityToMarginRatio": _stored(latest, "equity_to_margin_ratio"),
            # stored as a fraction of equity ((equity - margin) / equity); the report prints percent
            "marginCushion": None
            if _get(latest, "margin_cushion") is None
            else float(latest["margin_cushion"]) * 100,
            "totalNotional": _stored(latest, "gross_notional"),
            "unrealizedPnL": _stored(latest, "total_unrealized_pnl"),
            "realizedPnL": _stored(latest, "total_realized_pnl"),
            "totalCommissions": _stored(latest, "total_transaction_costs"),
            "netPnL": _stored(latest, "total_pnl"),
            "cashAvailable": _stored(latest, "cash_available"),
            "currentPortfolioValue": current_value,
        },
    }
