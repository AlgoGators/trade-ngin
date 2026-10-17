"""The command log, trading.position_overrides (docs/design/qt-contract.md section 4).

AlgoLens inserts rows with status 'pending'; the agent claims them and the engine (or the agent,
for the override e-mail) records the outcome. The table is both the log and the queue
(ruling 26). Every UPDATE here touches only the columns the 023 trigger lets change: status,
result, message, started_at, finished_at, token_hash and token_expires_at.

Claiming is `UPDATE ... SET status='running' WHERE id = %s AND status = 'pending' RETURNING ...`,
so two dispatchers (an RPC and the re-drive sweep, or two agents) can never both run a row.

The status moves the 025 trigger allows (contract C4): pending -> running; running -> done,
refused or failed; running -> pending only for the agent's recovery of a stale or orphaned row,
which sets `algogators.recovery = 'on'` for its own transaction (`requeue`). Terminal rows are
final.
"""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Protocol, Sequence, Tuple

from .config import CONNECT_TIMEOUT_S, DbConfig
from .store import StoreError

KINDS = ("save", "override_request", "override_decision", "publish", "rescind")
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

# What `rescind` found (contract C8). NOT_APPROVER: an override-published day, and the requester
# is neither the VP nor the President (Dom 2026-10-16).
RESCINDED, UNPUBLISHED, NOT_DESK, SENT, NOT_RUNNING, NOT_APPROVER = (
    "rescinded", "unpublished", "not_desk", "sent", "not_running", "not_approver")
# The C10 flow of an override's approval (commands.FLOWS).
OVERRIDE_FLOW = "override_b"


@dataclass(frozen=True)
class RescindResult:
    outcome: str  # one of RESCINDED, UNPUBLISHED, NOT_DESK, SENT, NOT_RUNNING, NOT_APPROVER
    published_by: Optional[str] = None
    published_at: Optional[dt.datetime] = None
    publish_source: Optional[str] = None
    sent_at: Optional[dt.datetime] = None
    # The approving publish row's C10 flow (payload.flow): "override_b" for an override, else
    # None for a plain publish, "auto" or "accept_c".
    flow: Optional[str] = None


def is_approver(email: Optional[str], approvers: Sequence[str]) -> bool:
    """Whether `email` is one of the override approvers (QT_APPROVERS), compared without case."""
    who = (email or "").strip().lower()
    return bool(who) and who in {a.strip().lower() for a in approvers if a}


class _NotRunning(StoreError):
    """Rolls the rescind back: its row was no longer running."""


class CommandStore(Protocol):
    def get_row(self, audit_id: int) -> Optional[CommandRow]: ...

    def claim(self, audit_id: int) -> Optional[CommandRow]:
        """pending -> running. None if the row is not pending (someone else has it)."""

    def finish(self, audit_id: int, status: str, result: Optional[Any],
               message: str) -> bool:
        """running -> done/refused/failed. False if the row was no longer running."""

    def record_mail(self, audit_id: int, token_hash: str, expires_at: dt.datetime,
                    result: Dict[str, Any]) -> Optional[dt.datetime]:
        """On a running override request: the token hash, its expiry (the book date's 10:00 New
        York cutoff, D-13) and the result, in one UPDATE (the mail has gone out). Returns the
        expiry, None if the row was no longer running."""

    def running_rows(self) -> List[Tuple[CommandRow, Optional[float]]]:
        """Every 'running' row with its age in seconds (now() - started_at, by the database's
        clock; None when started_at is NULL), oldest first."""

    def requeue(self, audit_id: int, message: str) -> bool:
        """running -> pending (the agent's recovery). False if the row was no longer running."""

    def pending_rows(self) -> List[CommandRow]: ...

    def decisions_for(self, parent_id: int) -> List[CommandRow]:
        """Every override_decision row whose parent is `parent_id`, oldest first."""

    def publishes_after(self, after_id: int) -> List[CommandRow]:
        """Every publish row whose C10 flow runs after row `after_id` (payload.after), oldest
        first."""

    def published_at(self, portfolio_id: str, date: dt.date) -> Optional[dt.datetime]:
        """live_run_metadata.published_at for the day (contract C3), None if unpublished."""

    def newest_pending_publish(self, portfolio_id: str, date: dt.date) -> Optional[CommandRow]: ...

    def books(self, portfolio_id: str, date: dt.date) -> Books: ...

    def rescind(self, audit_id: int, portfolio_id: str, date: dt.date,
                message: str, requested_by: str,
                approvers: Sequence[str] = ()) -> RescindResult:
        """Contract C8, in one transaction with the day's live_run_metadata rows locked: when the
        day is published by the desk (publish_source 'desk' on every published row) and no row
        has sent_at, clear published_by, published_at and publish_source on them and finish the
        running rescind row `audit_id` done with result {"rescinded": {published_by,
        published_at, flow}} and `message`. Who may (Dom 2026-10-16): anyone on QT takes back a
        desk approval (AlgoLens checks the QT screen); a day published through an approved
        override (the approving publish row's flow is override_b) only the VP or the President,
        `requested_by` among `approvers` (e-mails compared without case). Otherwise change nothing
        and say why (the caller refuses the row); NOT_RUNNING (nothing changed) when the row was
        no longer running."""


class PostgresCommandStore:
    def __init__(self, db: DbConfig, connect_timeout: int = CONNECT_TIMEOUT_S):
        self._db = db
        self._connect_timeout = connect_timeout

    def _connect(self, autocommit: bool = True):
        import psycopg

        return psycopg.connect(**self._db.conninfo_kwargs(self._connect_timeout),
                               autocommit=autocommit)

    def _run(self, fn, autocommit: bool = True):
        try:
            # Without autocommit, leaving the `with` commits (or rolls back on an exception).
            conn = self._connect() if autocommit else self._connect(False)
            with conn, conn.cursor() as cur:
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

    def record_mail(self, audit_id: int, token_hash: str, expires_at: dt.datetime,
                    result: Dict[str, Any]) -> Optional[dt.datetime]:
        def q(cur):
            from psycopg.types.json import Jsonb

            cur.execute("UPDATE trading.position_overrides "
                        "SET token_hash = %s, token_expires_at = %s, "
                        "result = %s "
                        "WHERE id = %s AND status = 'running' RETURNING token_expires_at",
                        (token_hash, expires_at, Jsonb(result), audit_id))
            rec = cur.fetchone()
            return rec[0] if rec else None
        return self._run(q)

    def running_rows(self) -> List[Tuple[CommandRow, Optional[float]]]:
        def q(cur):
            cur.execute(f"SELECT {_SELECT}, extract(epoch FROM now() - started_at)::float8 "
                        "FROM trading.position_overrides WHERE status = 'running' ORDER BY id")
            return [(CommandRow.from_record(r[:-1]), r[-1]) for r in cur.fetchall()]
        return self._run(q)

    def requeue(self, audit_id: int, message: str) -> bool:
        def q(cur):
            # The 025 trigger allows running -> pending only inside a transaction that set this.
            cur.execute("SET LOCAL algogators.recovery = 'on'")
            cur.execute("UPDATE trading.position_overrides "
                        "SET status = 'pending', started_at = NULL, finished_at = NULL, "
                        "message = %s WHERE id = %s AND status = 'running' RETURNING id",
                        (message, audit_id))
            return cur.fetchone() is not None
        return self._run(q, autocommit=False)

    def pending_rows(self) -> List[CommandRow]:
        def q(cur):
            cur.execute(f"SELECT {_SELECT} FROM trading.position_overrides "
                        "WHERE status = 'pending' ORDER BY id")
            return [CommandRow.from_record(r) for r in cur.fetchall()]
        return self._run(q)

    def decisions_for(self, parent_id: int) -> List[CommandRow]:
        def q(cur):
            cur.execute(f"SELECT {_SELECT} FROM trading.position_overrides "
                        "WHERE kind = 'override_decision' AND parent_id = %s ORDER BY id",
                        (parent_id,))
            return [CommandRow.from_record(r) for r in cur.fetchall()]
        return self._run(q)

    def publishes_after(self, after_id: int) -> List[CommandRow]:
        def q(cur):
            cur.execute(f"SELECT {_SELECT} FROM trading.position_overrides "
                        "WHERE kind = 'publish' AND payload->>'after' = %s ORDER BY id",
                        (str(after_id),))
            return [CommandRow.from_record(r) for r in cur.fetchall()]
        return self._run(q)

    def published_at(self, portfolio_id: str, date: dt.date) -> Optional[dt.datetime]:
        def q(cur):
            cur.execute("SELECT max(published_at) FROM trading.live_run_metadata "
                        "WHERE portfolio_id = %s AND date = %s", (portfolio_id, date))
            rec = cur.fetchone()
            return rec[0] if rec else None
        return self._run(q)

    def newest_pending_publish(self, portfolio_id: str, date: dt.date) -> Optional[CommandRow]:
        def q(cur):
            cur.execute(f"SELECT {_SELECT} FROM trading.position_overrides "
                        "WHERE kind = 'publish' AND status = 'pending' "
                        "AND portfolio_id = %s AND date = %s ORDER BY id DESC LIMIT 1",
                        (portfolio_id, date))
            return self._one(cur)
        return self._run(q)

    def rescind(self, audit_id: int, portfolio_id: str, date: dt.date,
                message: str, requested_by: str,
                approvers: Sequence[str] = ()) -> RescindResult:
        def q(cur):
            from psycopg.types.json import Jsonb

            cur.execute("SELECT published_by, published_at, publish_source, sent_at "
                        "FROM trading.live_run_metadata WHERE portfolio_id = %s AND date = %s "
                        "ORDER BY id FOR UPDATE", (portfolio_id, date))
            rows = cur.fetchall()
            sent = [r[3] for r in rows if r[3] is not None]
            if sent:
                return RescindResult(SENT, sent_at=max(sent))
            published = [r for r in rows if r[1] is not None]
            if not published:
                return RescindResult(UNPUBLISHED)
            by, at, source, _ = max(published, key=lambda r: r[1])
            if any(r[2] != "desk" for r in published):
                return RescindResult(NOT_DESK, by, at, source)
            # The row that approved the day: the newest done publish (a rescinded one is older).
            cur.execute("SELECT payload->>'flow' FROM trading.position_overrides "
                        "WHERE portfolio_id = %s AND date = %s AND kind = 'publish' "
                        "AND status = 'done' AND result ? 'published_at' "
                        "ORDER BY id DESC LIMIT 1", (portfolio_id, date))
            rec = cur.fetchone()
            flow = rec[0] if rec else None
            if flow == OVERRIDE_FLOW and not is_approver(requested_by, approvers):
                return RescindResult(NOT_APPROVER, by, at, source, flow=flow)
            cur.execute("UPDATE trading.live_run_metadata "
                        "SET published_by = NULL, published_at = NULL, publish_source = NULL "
                        "WHERE portfolio_id = %s AND date = %s AND published_at IS NOT NULL",
                        (portfolio_id, date))
            result = {"rescinded": {"published_by": by, "published_at": at.isoformat(),
                                    "flow": flow}}
            cur.execute("UPDATE trading.position_overrides "
                        "SET status = 'done', result = %s, message = %s, finished_at = now() "
                        "WHERE id = %s AND status = 'running' AND kind = 'rescind' RETURNING id",
                        (Jsonb(result), message, audit_id))
            if cur.fetchone() is None:
                raise _NotRunning("the rescind row is no longer running")
            return RescindResult(RESCINDED, by, at, source, flow=flow)
        try:
            return self._run(q, autocommit=False)
        except _NotRunning:
            return RescindResult(NOT_RUNNING)

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
