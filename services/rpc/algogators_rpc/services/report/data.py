"""Read one portfolio's report inputs from the trading database (psycopg 3).

Every read is of one day (the --date the engine sends) and one book: a QT portfolio keeps its own
'qt' rows next to the engine's 'system' rows, and the book served is 'qt' when the desk's book has
rows on that day, else 'system'. The stored figures are read as the engine wrote them; the report
recomputes none of them.
"""

from __future__ import annotations

import datetime as dt
from typing import Any

from .calc import parse_contract_size

DAYS = 30
QT, SYSTEM = "qt", "system"


class ReportDataError(RuntimeError):
    """The inputs for the report could not be read (the message is safe to log)."""


# The portfolio's strategy: the one the engine names (--strategy-id, the id it stored the day
# under), else the portfolio's first. A portfolio can register several (BASE_PORTFOLIO has two).
REGISTRY_SQL = """
SELECT id, strategy_type, portfolio_id, name, description, initial_equity
  FROM trading.strategy_registry
 WHERE portfolio_id = %(p)s AND (%(s)s::text IS NULL OR strategy_type = %(s)s::text)
 ORDER BY sort_order ASC, id ASC
 LIMIT 1
"""

# The served book: 'qt' when the desk's book has rows on the day, else the engine's 'system' book.
BOOK_HAS_DAY_SQL = """
SELECT 1 FROM trading.positions
 WHERE strategy_id = %(s)s AND portfolio_id = %(p)s AND portfolio_type = 'qt' AND date = %(d)s
 LIMIT 1
"""

# The day's stored row and the settled day's (the last row before it), of the served book.
RESULTS_SQL = """
SELECT * FROM trading.live_results
 WHERE strategy_id = %(s)s AND portfolio_id = %(p)s AND portfolio_type = %(b)s AND date = %(d)s
 ORDER BY created_at DESC
 LIMIT 1
"""
PREVIOUS_RESULTS_SQL = """
SELECT * FROM trading.live_results
 WHERE strategy_id = %(s)s AND portfolio_id = %(p)s AND portfolio_type = %(b)s AND date < %(d)s
 ORDER BY date DESC, created_at DESC
 LIMIT 1
"""

START_SQL = """
SELECT MIN(live_start_date) AS start FROM trading.strategy_trading_days_metadata
 WHERE strategy_id = %(s)s AND portfolio_id = %(p)s
"""

EQUITY_SQL = """
SELECT timestamp, equity FROM trading.equity_curve
 WHERE strategy_id = %(s)s AND portfolio_id = %(p)s AND portfolio_type = %(b)s
   AND timestamp::date <= %(d)s
 ORDER BY timestamp ASC
"""

# The book of one date, one row per sleeve and symbol (the latest written); a flatten (quantity 0)
# hides the row. Migration 040's columns are read through to_jsonb, so a database without them
# still answers (NULL).
_POSITIONS_OF = """
SELECT * FROM (
    SELECT DISTINCT ON (strategy_name, symbol)
           strategy_name, symbol, quantity, average_price, daily_unrealized_pnl,
           daily_realized_pnl,
           (to_jsonb(p) ->> 'mark_price')::double precision AS mark_price,
           (to_jsonb(p) ->> 'settle_from_price')::double precision AS settle_from_price,
           (to_jsonb(p) ->> 'settle_to_price')::double precision AS settle_to_price,
           to_jsonb(p) ->> 'settle_mark' AS settle_mark
      FROM trading.positions p
     WHERE strategy_id = %(s)s AND portfolio_id = %(p)s AND portfolio_type = %(b)s
       AND date = {date}
     ORDER BY strategy_name, symbol, updated_at DESC
) AS day_positions
"""
POSITIONS_SQL = _POSITIONS_OF.format(date="%(d)s") + """WHERE quantity <> 0
ORDER BY strategy_name, ABS(quantity * average_price) DESC
"""
# The settled day's book: the last stored date before the day, its rows as stored.
YESTERDAY_POSITIONS_SQL = _POSITIONS_OF.format(
    date="""(SELECT max(date) FROM trading.positions
                    WHERE strategy_id = %(s)s AND portfolio_id = %(p)s AND portfolio_type = %(b)s
                      AND date < %(d)s)"""
)

# The day's fills, strategy fills and roll legs, with the cost the account paid after netting.
EXECUTIONS_SQL = """
SELECT strategy_name, symbol, side, quantity, price, execution_time, commissions_fees,
       (to_jsonb(e) ->> 'total_transaction_costs')::double precision AS total_transaction_costs,
       COALESCE(to_jsonb(e) ->> 'execution_type', 'STRATEGY') AS execution_type,
       COALESCE((to_jsonb(e) ->> 'netting_adjustment')::numeric, 0) AS netting_adjustment
  FROM trading.executions e
 WHERE strategy_id = %(s)s AND portfolio_id = %(p)s AND portfolio_type = %(b)s AND date = %(d)s
 ORDER BY strategy_name, execution_time, symbol
"""

CONTRACT_SIZES_SQL = """
SELECT DISTINCT ON (cm."Databento Symbol")
       cm."Databento Symbol" AS root, cm."Contract Size" AS contract_size
  FROM metadata.contract_metadata cm
 WHERE cm."Databento Symbol" = ANY(%s)
 ORDER BY cm."Databento Symbol"
"""

# The futures runners leave gross_leverage NULL and write portfolio_leverage, so gross falls back.
HISTORY_SQL = f"""
SELECT date, COALESCE(gross_leverage, portfolio_leverage), net_leverage, margin_posted
  FROM trading.live_results
 WHERE portfolio_id = %(p)s AND strategy_id = %(s)s AND portfolio_type = %(b)s AND date <= %(d)s
 ORDER BY date DESC
 LIMIT {DAYS}
"""
CONTRACTS_SQL = """
SELECT date, SUM(ABS(quantity))
  FROM trading.executions
 WHERE portfolio_id = %(p)s AND strategy_id = %(s)s AND portfolio_type = %(b)s
   AND date >= %(since)s AND date <= %(d)s
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


def fetch_registry(cur, portfolio_id: str, strategy_id: str | None = None) -> dict[str, Any]:
    cur.execute(REGISTRY_SQL, {"p": portfolio_id, "s": strategy_id or None})
    row = cur.fetchone()
    if row is None and strategy_id:
        # The engine's strategy is not registered under that id: the portfolio's row gives the
        # name and the capital, and the day is still read under the id it was stored under.
        cur.execute(REGISTRY_SQL, {"p": portfolio_id, "s": None})
        row = cur.fetchone()
        if row is not None:
            return {**dict(row), "strategy_type": strategy_id}
    if row is None:
        raise ReportDataError(f"portfolio {portfolio_id} is not in trading.strategy_registry")
    return dict(row)


def fetch_detail_rows(cur, strategy_id: str, portfolio_id: str, day: dt.date) -> dict[str, Any]:
    """The day's inputs, all of the served book: latest (the day's live_results row), previous
    (the settled day's row), equity_curve, positions, executions, yesterday_positions,
    contract_sizes, book and start (the book's live start date, or None)."""
    params = {"s": strategy_id, "p": portfolio_id, "d": day}
    cur.execute(BOOK_HAS_DAY_SQL, params)
    book = QT if cur.fetchone() is not None else SYSTEM
    params["b"] = book

    cur.execute(RESULTS_SQL, params)
    latest = cur.fetchone()
    if not latest:
        return {"latest": None, "book": book}
    cur.execute(PREVIOUS_RESULTS_SQL, params)
    previous = cur.fetchone()
    cur.execute(START_SQL, params)
    start_row = cur.fetchone()

    cur.execute(EQUITY_SQL, params)
    equity_curve = cur.fetchall()
    cur.execute(POSITIONS_SQL, params)
    positions = cur.fetchall()
    cur.execute(EXECUTIONS_SQL, params)
    executions = cur.fetchall()
    cur.execute(YESTERDAY_POSITIONS_SQL, params)
    yesterday = cur.fetchall()

    return {
        "latest": latest,
        "previous": previous,
        "equity_curve": equity_curve,
        "positions": positions,
        "executions": executions,
        "yesterday_positions": yesterday,
        "contract_sizes": _contract_sizes(cur, positions, executions),
        "book": book,
        "start": start_row["start"] if start_row else None,
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


def fetch_history(cur, portfolio_id: str, strategy_id: str, book: str, day: dt.date) -> dict:
    """Leverage, margin and contracts per day for the DAYS days through `day` (tuple cursor)."""
    params = {"p": portfolio_id, "s": strategy_id, "b": book, "d": day}
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
