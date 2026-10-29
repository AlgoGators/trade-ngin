"""Portfolio detail from stored rows: the pure transforms the daily report is built on.

Ported from AlgoLens (research-api domain/portfolio/calculations.py, research-core returns.py and
build_strategy_detail), which this module replaces as the one place the report's numbers are made.
Stdlib only.
"""

from __future__ import annotations

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


def transform_positions(
    positions: Sequence[Any], current_value: float, sizes: Mapping[str, float] | None = None
) -> list[dict[str, Any]]:
    out = []
    for pos in positions:
        quantity = float(_get(pos, "quantity"))
        average_price = float(_get(pos, "average_price"))
        notional = abs(quantity * average_price * contract_size(_get(pos, "symbol"), sizes))
        out.append(
            {
                "symbol": _get(pos, "symbol"),
                "name": _get(pos, "symbol").replace(".v.0", ""),
                "shares": quantity,
                "quantity": quantity,
                "costBasis": average_price,
                "currentValue": notional,
                "marketPrice": average_price,
                "notional": notional,
                "percentOfTotal": (notional / current_value * 100) if current_value > 0 else 0,
            }
        )
    return out


def transform_executions(
    executions: Sequence[Any], sizes: Mapping[str, float] | None = None
) -> list[dict[str, Any]]:
    out = []
    for execution in executions:
        exec_time = _get(execution, "execution_time")
        quantity = float(_get(execution, "quantity"))
        price = float(_get(execution, "price"))
        size = contract_size(_get(execution, "symbol"), sizes)
        out.append(
            {
                "symbol": _get(execution, "symbol"),
                "side": _get(execution, "side"),
                "quantity": quantity,
                "price": price,
                "notional": quantity * price * size,
                # what the account paid: the row's own cost less its netting adjustment (013)
                "commission": float(_get(execution, "commissions_fees") or 0)
                - float(_get(execution, "netting_adjustment") or 0),
                "type": str(_get(execution, "execution_type") or "STRATEGY").upper(),
                "date": exec_time.isoformat() if exec_time else None,
            }
        )
    return out


def transform_finalized(
    yesterday_positions: Sequence[Any], positions: Sequence[Any]
) -> list[dict[str, Any]]:
    """The settled day's book, every held row, with the settle's P&L as stored on the row
    (daily_realized_pnl, written when the next run settles the day). The two prices are the
    row's mark on the settled day and today's mark of the same symbol (positions.average_price)."""
    today = {_get(p, "symbol"): p for p in positions}
    out = []
    for yesterday in yesterday_positions:
        quantity = float(_get(yesterday, "quantity"))
        if quantity == 0:
            continue
        symbol = _get(yesterday, "symbol")
        previous_price = float(_get(yesterday, "average_price"))
        today_pos = today.get(symbol)
        out.append(
            {
                "symbol": symbol.replace(".v.0", ""),
                "quantity": quantity,
                "entryPrice": previous_price,
                "exitPrice": float(_get(today_pos, "average_price")) if today_pos else previous_price,
                "realizedPnL": float(_get(yesterday, "daily_realized_pnl") or 0),
            }
        )
    return sorted(out, key=lambda r: r["symbol"])


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
        "return": float_or_default(latest["total_pnl"]),
        "returnPercent": float_or_default(latest["total_cumulative_return"]),
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
            "volatility": float_or_default(latest["volatility"]),
            "sharpeRatio": float_or_default(latest["sharpe_ratio"]),
            "sortinoRatio": float_or_default(latest["sortino_ratio"]),
            "maxDrawdown": float_or_default(latest["max_drawdown"]),
            "totalTrades": sum(1 for e in transformed_executions if e["type"] == "STRATEGY"),
            "cumulativeReturn": float_or_default(latest["total_cumulative_return"]),
            "annualizedReturn": float_or_default(latest["total_annualized_return"]),
            "totalRollCosts": _stored(latest, "total_roll_costs"),
            # the futures runners write gross leverage to portfolio_leverage and leave
            # gross_leverage NULL
            "grossLeverage": float_or_default(
                latest["gross_leverage"]
                if latest["gross_leverage"] is not None
                else latest["portfolio_leverage"]
            ),
            "netLeverage": float_or_default(latest["net_leverage"]),
            "portfolioLeverage": float_or_default(latest["portfolio_leverage"]),
            "marginPosted": float_or_default(latest["margin_posted"]),
            "equityToMarginRatio": float_or_default(latest["equity_to_margin_ratio"]),
            # stored as a fraction of equity ((equity - margin) / equity); the report prints percent
            "marginCushion": float_or_default(latest["margin_cushion"]) * 100,
            "totalNotional": float_or_default(latest["gross_notional"]),
            "unrealizedPnL": float_or_default(latest["total_unrealized_pnl"]),
            "realizedPnL": float_or_default(latest["total_realized_pnl"]),
            "totalCommissions": float_or_default(latest["total_transaction_costs"]),
            "netPnL": float_or_default(latest["total_pnl"]),
            "cashAvailable": float_or_default(latest["cash_available"]),
            "currentPortfolioValue": current_value,
        },
    }
