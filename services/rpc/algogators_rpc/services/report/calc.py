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
                "commission": float(_get(execution, "commissions_fees")),
                "date": exec_time.isoformat() if exec_time else None,
            }
        )
    return out


def transform_finalized(
    yesterday_positions: Sequence[Any], positions: Sequence[Any]
) -> list[dict[str, Any]]:
    """Yesterday's positions against today's: the lots that closed or changed."""
    out = []
    for yesterday in yesterday_positions:
        symbol = _get(yesterday, "symbol")
        yesterday_qty = float(_get(yesterday, "quantity"))
        yesterday_price = float(_get(yesterday, "average_price"))

        today_pos = next((p for p in positions if _get(p, "symbol") == symbol), None)
        today_qty = float(_get(today_pos, "quantity")) if today_pos else 0
        today_price = float(_get(today_pos, "average_price")) if today_pos else yesterday_price

        if today_qty - yesterday_qty != 0:
            out.append(
                {
                    "symbol": symbol.replace(".v.0", ""),
                    "quantity": yesterday_qty,
                    "entryPrice": yesterday_price,
                    "exitPrice": today_price,
                    "realizedPnL": float(_get(yesterday, "daily_realized_pnl") or 0),
                }
            )
    return out


def compute_return_stats(historical_data: Sequence[Mapping[str, Any]]) -> dict[str, float]:
    """Best/worst day, drawdown, win rate, and win/loss aggregates."""
    daily_returns = []
    daily_pnl = []
    for i in range(1, len(historical_data)):
        prev_val = historical_data[i - 1]["value"]
        curr_val = historical_data[i]["value"]
        if prev_val > 0:
            daily_returns.append(((curr_val - prev_val) / prev_val) * 100)
            daily_pnl.append(curr_val - prev_val)

    best_day = max(daily_returns) if daily_returns else 0
    worst_day = min(daily_returns) if daily_returns else 0

    max_drawdown = 0
    peak = historical_data[0]["value"] if historical_data else 0
    for point in historical_data:
        if point["value"] > peak:
            peak = point["value"]
        drawdown = ((peak - point["value"]) / peak) * 100 if peak > 0 else 0
        if drawdown > max_drawdown:
            max_drawdown = drawdown

    winning = [p for p in daily_pnl if p > 0]
    losing = [p for p in daily_pnl if p < 0]
    total_days = len(daily_pnl)
    gross_profit = sum(winning) if winning else 0
    gross_loss = abs(sum(losing)) if losing else 0
    return {
        "best_day": best_day,
        "worst_day": worst_day,
        "max_drawdown": max_drawdown,
        "win_rate": (len(winning) / total_days * 100) if total_days > 0 else 0,
        "avg_win": sum(winning) / len(winning) if winning else 0,
        "avg_loss": abs(sum(losing) / len(losing)) if losing else 0,
        "profit_factor": (gross_profit / gross_loss) if gross_loss > 0 else 0,
    }


def compute_sharpe(annualized_return: float, volatility: float) -> float:
    return annualized_return / volatility if volatility > 0 else 0


def build_strategy_detail(cfg: Mapping[str, Any], rows: Mapping[str, Any]) -> dict[str, Any] | None:
    """The report's view of one portfolio. `rows` holds latest (a live_results row), equity_curve,
    positions, executions, yesterday_positions, contract_sizes and book. None: no results yet."""
    latest = rows.get("latest")
    if not latest:
        return None

    equity_curve = rows.get("equity_curve") or []
    initial_equity = resolve_initial_equity(equity_curve, float(cfg["initial_equity"]))
    current_value = float(latest["current_portfolio_value"])
    total_return = current_value - initial_equity
    return_percent = (total_return / initial_equity * 100) if initial_equity > 0 else 0

    historical_data = build_historical_data(equity_curve)
    sizes = rows.get("contract_sizes")
    positions = rows.get("positions") or []
    transformed_executions = transform_executions(rows.get("executions") or [], sizes)
    stats = compute_return_stats(historical_data)

    volatility = float(latest["volatility"])
    annualized_return = float(latest["total_annualized_return"])

    return {
        "id": cfg["id"],
        "name": cfg["name"],
        "description": cfg.get("description"),
        "invested": initial_equity,
        "currentValue": current_value,
        "return": total_return,
        "returnPercent": return_percent,
        "positions": transform_positions(positions, current_value, sizes),
        "historicalData": historical_data,
        "bestDay": stats["best_day"],
        "worstDay": stats["worst_day"],
        "executions": transformed_executions,
        "finalizedPositions": transform_finalized(rows.get("yesterday_positions") or [], positions),
        "lastUpdate": latest["date"].isoformat(),
        "book": rows.get("book"),
        "metrics": {
            "volatility": volatility,
            "sharpeRatio": compute_sharpe(annualized_return, volatility),
            "maxDrawdown": stats["max_drawdown"],
            "winRate": stats["win_rate"],
            "totalTrades": len(transformed_executions),
            "avgWin": stats["avg_win"],
            "avgLoss": stats["avg_loss"],
            "profitFactor": stats["profit_factor"],
            "dailyReturn": float_or_default(latest["daily_return"]),
            "cumulativeReturn": return_percent,
            "annualizedReturn": annualized_return,
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
            "netPnL": total_return,
            "cashAvailable": float_or_default(latest["cash_available"]),
            "currentPortfolioValue": current_value,
        },
    }
