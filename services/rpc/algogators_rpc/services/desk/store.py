"""Read side of GetRunStatus: the facts Postgres holds about one book's run on one date.

The servicer depends only on the RunStatusStore protocol, so tests pass a fake. The Postgres
implementation reads:

* trading.live_run_metadata rows for (portfolio_id, date). Columns on main
  (PostgresDatabase::store_live_run_metadata): date, strategy_id, portfolio_id,
  strategy_allocations, portfolio_config, strategy_configs, plus created_at. A run's refusal is
  a key in portfolio_config (`risk_refusal`, `strict_assertion`; run_metadata_marks.hpp).
  published_by / published_at arrive with the publish migration (plan E8); until then they are
  absent and reported as such.
* trading.live_results for the same key: a completed run writes that row last
  (scripts/check_live_trading.py), so its presence is the "finished" signal.

Optional columns are discovered from information_schema on every call, so a migration that
adds them takes effect without restarting the agent.
"""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass, field
from typing import Optional, Protocol, Sequence

from .config import DbConfig

METADATA_TABLE = ("trading", "live_run_metadata")
RESULTS_TABLE = ("trading", "live_results")
_OPTIONAL_METADATA_COLUMNS = ("created_at", "published_by", "published_at")


class StoreError(RuntimeError):
    """The database could not answer. The message is safe to return to the caller."""


@dataclass(frozen=True)
class MetadataRow:
    portfolio_config: Optional[dict]
    created_at: Optional[dt.datetime] = None
    published_by: Optional[str] = None
    published_at: Optional[dt.datetime] = None


@dataclass(frozen=True)
class RunFacts:
    metadata: Sequence[MetadataRow] = field(default_factory=tuple)
    # True when the publish columns exist on live_run_metadata.
    has_publish_columns: bool = False
    has_results: bool = False
    results_written_at: Optional[dt.datetime] = None


class RunStatusStore(Protocol):
    def run_facts(self, portfolio_id: str, date: dt.date) -> RunFacts: ...


class PostgresRunStatusStore:
    def __init__(self, db: DbConfig, connect_timeout: int = 10):
        self._db = db
        self._connect_timeout = connect_timeout

    def _connect(self):
        import psycopg  # imported here so the pure parts of the package need no driver

        return psycopg.connect(**self._db.conninfo_kwargs(self._connect_timeout),
                               autocommit=True)

    @staticmethod
    def _columns(cur, schema: str, table: str) -> set:
        cur.execute("SELECT column_name FROM information_schema.columns "
                    "WHERE table_schema = %s AND table_name = %s", (schema, table))
        return {r[0] for r in cur.fetchall()}

    def run_facts(self, portfolio_id: str, date: dt.date) -> RunFacts:
        from psycopg import sql

        try:
            with self._connect() as conn, conn.cursor() as cur:
                meta_cols = self._columns(cur, *METADATA_TABLE)
                if not meta_cols:
                    raise StoreError("table trading.live_run_metadata not found")
                present = [c for c in _OPTIONAL_METADATA_COLUMNS if c in meta_cols]
                # Timestamps are read as timestamptz (a no-op on a timestamptz column; on a plain
                # timestamp, through the session zone, as the watchdog does: R12).
                select = [sql.Identifier("portfolio_config")] + [
                    sql.SQL("{}::timestamptz").format(sql.Identifier(c)) if c.endswith("_at")
                    else sql.Identifier(c) for c in present]
                order = sql.SQL(" ORDER BY created_at") if "created_at" in present else sql.SQL("")
                cur.execute(
                    sql.SQL("SELECT {} FROM trading.live_run_metadata "
                            "WHERE portfolio_id = %s AND date = %s{}")
                    .format(sql.SQL(", ").join(select), order),
                    (portfolio_id, date))
                rows = []
                for rec in cur.fetchall():
                    values = dict(zip(["portfolio_config"] + present, rec))
                    rows.append(MetadataRow(
                        portfolio_config=values["portfolio_config"],
                        created_at=values.get("created_at"),
                        published_by=values.get("published_by"),
                        published_at=values.get("published_at")))

                res_cols = self._columns(cur, *RESULTS_TABLE)
                has_results, written_at = False, None
                if res_cols:
                    if "created_at" in res_cols:
                        # created_at is timestamp without time zone, written in the writer's
                        # session zone; read through this session's zone to UTC (R12, as the
                        # watchdog does).
                        cur.execute("SELECT count(*), max(created_at)::timestamptz "
                                    "FROM trading.live_results "
                                    "WHERE portfolio_id = %s AND date = %s", (portfolio_id, date))
                        n, written_at = cur.fetchone()
                    else:
                        cur.execute("SELECT count(*) FROM trading.live_results "
                                    "WHERE portfolio_id = %s AND date = %s", (portfolio_id, date))
                        n = cur.fetchone()[0]
                    has_results = n > 0
                return RunFacts(metadata=tuple(rows),
                                has_publish_columns={"published_by", "published_at"} <= set(present),
                                has_results=has_results, results_written_at=written_at)
        except StoreError:
            raise
        except Exception as exc:  # psycopg errors carry no credentials, but keep only the class
            raise StoreError(f"database error ({type(exc).__name__})") from exc
