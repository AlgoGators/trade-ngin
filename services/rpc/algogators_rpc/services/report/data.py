"""Read one portfolio's report inputs from the trading database (psycopg 3).

The queries are AlgoLens's (research-api PostgresPortfolioRepository, PostgresReportHistory,
PostgresContractSymbols), keyed by portfolio id. A QT portfolio keeps its own 'qt' rows next to the
engine's 'system' rows; the book served is 'qt' when it has any position rows, else 'system'.
"""

from __future__ import annotations

import datetime as dt
from typing import Any

from .calc import parse_contract_size

DAYS = 30
QT, SYSTEM = "qt", "system"


class ReportDataError(RuntimeError):
    """The inputs for the report could not be read (the message is safe to log)."""


REGISTRY_SQL = """
SELECT id, strategy_type, portfolio_id, name, description, initial_equity
  FROM trading.strategy_registry
 WHERE portfolio_id = %s
 ORDER BY sort_order ASC, id ASC
 LIMIT 1
"""

LATEST_RESULTS_SQL = """
SELECT * FROM trading.live_results
 WHERE config::jsonb->>'strategy_type' = %s AND portfolio_id = %s
   AND portfolio_type IN (%s, 'system')
 ORDER BY date DESC, (portfolio_type = %s) DESC
 LIMIT 1
"""

BOOK_HAS_POSITIONS_SQL = """
SELECT 1 FROM trading.positions
 WHERE strategy_id = %s AND portfolio_id = %s AND portfolio_type = %s
 LIMIT 1
"""

EQUITY_SQL = """
SELECT timestamp, equity FROM trading.equity_curve
 WHERE strategy_id = %s AND portfolio_id = %s AND portfolio_type = %s
 ORDER BY timestamp ASC
"""

# The zero-quantity filter runs in the OUTER query, after the latest row per symbol is chosen, so
# a flatten (a quantity-0 row on the latest date) hides the symbol instead of resurrecting an
# older non-zero row.
POSITIONS_SQL = """
SELECT * FROM (
    SELECT DISTINCT ON (symbol)
           symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl
      FROM trading.positions
     WHERE strategy_id = %s AND portfolio_id = %s AND portfolio_type = %s
       AND date = (SELECT max(date) FROM trading.positions
                    WHERE strategy_id = %s AND portfolio_id = %s AND portfolio_type = %s)
     ORDER BY symbol, updated_at DESC
) AS latest_positions
WHERE quantity <> 0
ORDER BY ABS(quantity * average_price) DESC
"""

YESTERDAY_POSITIONS_SQL = """
SELECT DISTINCT ON (symbol)
       symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl
  FROM trading.positions
 WHERE strategy_id = %s AND portfolio_id = %s AND portfolio_type = %s
   AND date = (
        SELECT max(date) FROM trading.positions
         WHERE strategy_id = %s AND portfolio_id = %s AND portfolio_type = %s
           AND date < (SELECT max(date) FROM trading.positions
                        WHERE strategy_id = %s AND portfolio_id = %s AND portfolio_type = %s))
 ORDER BY symbol, updated_at DESC
"""

EXECUTIONS_SQL = """
SELECT symbol, side, quantity, price, execution_time, commissions_fees
  FROM trading.executions
 WHERE strategy_id = %s AND portfolio_id = %s AND portfolio_type = %s
 ORDER BY execution_time DESC
 LIMIT 100
"""

CONTRACT_SIZES_SQL = """
SELECT DISTINCT ON (cm."Databento Symbol")
       cm."Databento Symbol" AS root, cm."Contract Size" AS contract_size
  FROM metadata.contract_metadata cm
 WHERE cm."Databento Symbol" = ANY(%s)
 ORDER BY cm."Databento Symbol"
"""

# The futures runners leave gross_leverage NULL and write portfolio_leverage, so gross falls back.
_BOOK_OF_RESULTS = """
(SELECT CASE WHEN EXISTS (
          SELECT 1 FROM trading.live_results
           WHERE portfolio_id = %(p)s AND strategy_id = %(s)s AND portfolio_type = 'qt')
        THEN 'qt' ELSE 'system' END)
"""
HISTORY_SQL = f"""
SELECT date, COALESCE(gross_leverage, portfolio_leverage), net_leverage, margin_posted
  FROM trading.live_results
 WHERE portfolio_id = %(p)s AND strategy_id = %(s)s AND portfolio_type = {_BOOK_OF_RESULTS}
 ORDER BY date DESC
 LIMIT {DAYS}
"""
CONTRACTS_SQL = f"""
SELECT date, SUM(quantity)
  FROM trading.executions
 WHERE portfolio_id = %(p)s AND strategy_id = %(s)s AND portfolio_type = {_BOOK_OF_RESULTS}
   AND date >= %(since)s
 GROUP BY date
 ORDER BY date
"""

# Contract Months is read through to_jsonb so a database without that column still answers.
SYMBOLS_SQL = """
SELECT m."Databento Symbol", m."IB Symbol", m."Name",
       COALESCE(to_jsonb(m) ->> 'Contract Months', '')
  FROM metadata.contract_metadata m
 WHERE m."Databento Symbol" = ANY(%(s)s) OR m."IB Symbol" = ANY(%(s)s)
 ORDER BY m."Name"
"""


def _num(value):
    return None if value is None else float(value)


def _points(rows, index):
    return [
        {"date": r[0].isoformat(), "value": _num(r[index])} for r in rows if r[index] is not None
    ]


def fetch_registry(cur, portfolio_id: str) -> dict[str, Any]:
    cur.execute(REGISTRY_SQL, (portfolio_id,))
    row = cur.fetchone()
    if row is None:
        raise ReportDataError(f"portfolio {portfolio_id} is not in trading.strategy_registry")
    return dict(row)


def fetch_detail_rows(cur, strategy_type: str, portfolio_id: str) -> dict[str, Any]:
    """latest, equity_curve, positions, executions, yesterday_positions, contract_sizes, book."""
    cur.execute(LATEST_RESULTS_SQL, (strategy_type, portfolio_id, QT, QT))
    latest = cur.fetchone()
    if not latest:
        return {"latest": None}

    cur.execute(BOOK_HAS_POSITIONS_SQL, (strategy_type, portfolio_id, QT))
    book = QT if cur.fetchone() is not None else SYSTEM

    cur.execute(EQUITY_SQL, (strategy_type, portfolio_id, book))
    equity_curve = cur.fetchall()
    cur.execute(POSITIONS_SQL, (strategy_type, portfolio_id, book) * 2)
    positions = cur.fetchall()
    cur.execute(EXECUTIONS_SQL, (strategy_type, portfolio_id, book))
    executions = cur.fetchall()
    cur.execute(YESTERDAY_POSITIONS_SQL, (strategy_type, portfolio_id, book) * 3)
    yesterday = cur.fetchall()

    return {
        "latest": latest,
        "equity_curve": equity_curve,
        "positions": positions,
        "executions": executions,
        "yesterday_positions": yesterday,
        "contract_sizes": _contract_sizes(cur, positions, executions),
        "book": book,
    }


def _contract_sizes(cur, *row_lists) -> dict[str, float]:
    roots = sorted(
        {(row["symbol"] or "").split(".", 1)[0] for rows in row_lists for row in rows}
    )
    if not roots:
        return {}
    cur.execute("SELECT to_regclass('metadata.contract_metadata') IS NOT NULL AS present")
    present = cur.fetchone()
    if not present or not present["present"]:
        return {}
    cur.execute(CONTRACT_SIZES_SQL, (roots,))
    sizes = {}
    for row in cur.fetchall():
        size = parse_contract_size(row["contract_size"])
        if size:
            sizes[row["root"]] = size
    return sizes


def fetch_history(cur, portfolio_id: str, strategy_type: str) -> dict[str, list]:
    """Leverage, margin and contracts per day for the last DAYS days (tuple cursor)."""
    params = {"p": portfolio_id, "s": strategy_type}
    cur.execute(HISTORY_SQL, params)
    rows = list(reversed(cur.fetchall()))
    contracts: list = []
    if rows:
        cur.execute(CONTRACTS_SQL, {**params, "since": rows[0][0]})
        contracts = cur.fetchall()
    return {
        "leverage": _points(rows, 1),
        "net_leverage": _points(rows, 2),
        "margin_posted": _points(rows, 3),
        "contracts_per_day": _points(contracts, 1),
    }


def fetch_symbols(cur, roots) -> dict[str, list]:
    """The symbols reference: {"rows": [{db, ib, name, months}], "missing": [roots with no row]}."""
    roots = sorted(set(roots))
    if not roots:
        return {"rows": [], "missing": []}
    cur.execute(SYMBOLS_SQL, {"s": roots})
    rows = [
        {"db": r[0], "ib": r[1] or "", "name": r[2] or "", "months": r[3] or ""}
        for r in cur.fetchall()
    ]
    known = {r["db"] for r in rows} | {r["ib"] for r in rows}
    return {"rows": rows, "missing": [s for s in roots if s not in known]}


def report_day(day: str) -> dt.date:
    try:
        return dt.date.fromisoformat(day)
    except ValueError:
        raise ReportDataError(f"not a date: {day!r}") from None
