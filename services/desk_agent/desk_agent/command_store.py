"""The command log, trading.position_overrides (docs/design/qt-contract.md section 4).

AlgoLens inserts rows with status 'pending'; the agent claims them and the engine (or the agent,
for the override e-mail) records the outcome. The table is both the log and the queue
(ruling 26). Every UPDATE here touches only the columns the 023 trigger lets change: status,
result, message, started_at, finished_at, token_hash and token_expires_at.

Claiming is `UPDATE ... SET status='running' WHERE id = %s AND status = 'pending' RETURNING ...`,
so two dispatchers (an RPC and the re-drive sweep, or two agents) can never both run a row.
"""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Protocol

from .config import DbConfig
from .store import StoreError

KINDS = ("save", "override_request", "override_decision", "publish")
FINAL = ("done", "refused", "failed")

COLUMNS = ("id", "portfolio_id", "date", "kind", "status", "requested_by", "reason", "payload",
           "parent_id", "approver_role", "token_hash", "token_expires_at", "result", "message",
           "created_at", "started_at", "finished_at")
_SELECT = ", ".join(COLUMNS)
BOOKS = ("system", "qt_proposal", "qt")


@dataclass(frozen=True)
class CommandRow:
    id: int
    portfolio_id: str
    date: dt.date
    kind: str
    status: str
    requested_by: str
    reason: Optional[str] = None
    payload: Dict[str, Any] = field(default_factory=dict)
    parent_id: Optional[int] = None
    approver_role: Optional[str] = None
    token_hash: Optional[str] = field(default=None, repr=False)
    token_expires_at: Optional[dt.datetime] = None
    result: Optional[Any] = None
    message: Optional[str] = None
    created_at: Optional[dt.datetime] = None
    started_at: Optional[dt.datetime] = None
    finished_at: Optional[dt.datetime] = None

    @classmethod
    def from_record(cls, rec) -> "CommandRow":
        values = dict(zip(COLUMNS, rec))
        if values.get("payload") is None:
            values["payload"] = {}
        return cls(**values)


@dataclass(frozen=True)
class BookLine:
    quantity: float
    moved_by: Optional[str] = None


# {portfolio_type: {symbol: BookLine}}
Books = Dict[str, Dict[str, BookLine]]


class CommandStore(Protocol):
    def get_row(self, audit_id: int) -> Optional[CommandRow]: ...

    def claim(self, audit_id: int) -> Optional[CommandRow]:
        """pending -> running. None if the row is not pending (someone else has it)."""

    def finish(self, audit_id: int, status: str, result: Optional[Any],
               message: str) -> bool:
        """running -> done/refused/failed. False if the row was no longer running."""

    def set_token(self, audit_id: int, token_hash: str, hours: int) -> Optional[dt.datetime]:
        """Store the token hash and an expiry `hours` from now(); returns the expiry."""

    def reset_running(self) -> List[int]:
        """running -> pending for every row (orphans of a previous agent). Returns their ids."""

    def pending_rows(self) -> List[CommandRow]: ...

    def newest_pending_publish(self, portfolio_id: str, date: dt.date) -> Optional[CommandRow]: ...

    def books(self, portfolio_id: str, date: dt.date) -> Books: ...


class PostgresCommandStore:
    def __init__(self, db: DbConfig, connect_timeout: int = 10):
        self._db = db
        self._connect_timeout = connect_timeout

    def _connect(self):
        import psycopg

        return psycopg.connect(**self._db.conninfo_kwargs(self._connect_timeout),
                               autocommit=True)

    def _run(self, fn):
        try:
            with self._connect() as conn, conn.cursor() as cur:
                return fn(cur)
        except StoreError:
            raise
        except Exception as exc:  # keep only the class: no credentials, no row contents
            raise StoreError(f"database error ({type(exc).__name__})") from exc

    @staticmethod
    def _one(cur) -> Optional[CommandRow]:
        rec = cur.fetchone()
        return CommandRow.from_record(rec) if rec else None

    def get_row(self, audit_id: int) -> Optional[CommandRow]:
        def q(cur):
            cur.execute(f"SELECT {_SELECT} FROM trading.position_overrides WHERE id = %s",
                        (audit_id,))
            return self._one(cur)
        return self._run(q)

    def claim(self, audit_id: int) -> Optional[CommandRow]:
        def q(cur):
            cur.execute("UPDATE trading.position_overrides "
                        "SET status = 'running', started_at = now() "
                        f"WHERE id = %s AND status = 'pending' RETURNING {_SELECT}",
                        (audit_id,))
            return self._one(cur)
        return self._run(q)

    def finish(self, audit_id: int, status: str, result: Optional[Any], message: str) -> bool:
        if status not in FINAL:
            raise ValueError(f"not a final status: {status}")

        def q(cur):
            from psycopg.types.json import Jsonb

            cur.execute("UPDATE trading.position_overrides "
                        "SET status = %s, result = %s, message = %s, finished_at = now() "
                        "WHERE id = %s AND status = 'running' RETURNING id",
                        (status, None if result is None else Jsonb(result), message, audit_id))
            return cur.fetchone() is not None
        return self._run(q)

    def set_token(self, audit_id: int, token_hash: str, hours: int) -> Optional[dt.datetime]:
        def q(cur):
            cur.execute("UPDATE trading.position_overrides "
                        "SET token_hash = %s, token_expires_at = now() + make_interval(hours => %s) "
                        "WHERE id = %s AND status = 'running' RETURNING token_expires_at",
                        (token_hash, hours, audit_id))
            rec = cur.fetchone()
            return rec[0] if rec else None
        return self._run(q)

    def reset_running(self) -> List[int]:
        def q(cur):
            cur.execute("UPDATE trading.position_overrides "
                        "SET status = 'pending', started_at = NULL, "
                        "message = 're-driven after agent restart' "
                        "WHERE status = 'running' RETURNING id")
            return sorted(r[0] for r in cur.fetchall())
        return self._run(q)

    def pending_rows(self) -> List[CommandRow]:
        def q(cur):
            cur.execute(f"SELECT {_SELECT} FROM trading.position_overrides "
                        "WHERE status = 'pending' ORDER BY id")
            return [CommandRow.from_record(r) for r in cur.fetchall()]
        return self._run(q)

    def newest_pending_publish(self, portfolio_id: str, date: dt.date) -> Optional[CommandRow]:
        def q(cur):
            cur.execute(f"SELECT {_SELECT} FROM trading.position_overrides "
                        "WHERE kind = 'publish' AND status = 'pending' "
                        "AND portfolio_id = %s AND date = %s ORDER BY id DESC LIMIT 1",
                        (portfolio_id, date))
            return self._one(cur)
        return self._run(q)

    def books(self, portfolio_id: str, date: dt.date) -> Books:
        def q(cur):
            cur.execute("SELECT portfolio_type, symbol, sum(quantity), max(moved_by) "
                        "FROM trading.positions "
                        "WHERE portfolio_id = %s AND date = %s "
                        "AND portfolio_type IN ('system', 'qt_proposal', 'qt') "
                        "GROUP BY portfolio_type, symbol ORDER BY portfolio_type, symbol",
                        (portfolio_id, date))
            out: Books = {b: {} for b in BOOKS}
            for book, symbol, qty, moved_by in cur.fetchall():
                out.setdefault(book, {})[symbol] = BookLine(float(qty or 0), moved_by)
            return out
        return self._run(q)
