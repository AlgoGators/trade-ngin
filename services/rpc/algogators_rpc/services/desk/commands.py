"""Command dispatch: from a trading.position_overrides row to the work that answers it.

Used by the RPCs (the fast path) and by the re-drive sweep (the path that never loses a row).
Both go through `claim`, which only succeeds on a 'pending' row, so a row runs at most once at a
time.

| kind              | work                                                              |
|-------------------|-------------------------------------------------------------------|
| save              | engine `--desk` with the save row's id                            |
| override_request  | the override e-mail, in Python (override_mail.py)                 |
| override_decision | checks here; rejection -> done {"approved": false}; approval ->   |
|                   | engine `--override` with the decision row's id                    |
| publish           | engine `--publish` with the publish row's id                      |

Ownership. This process owns a row from the moment a dispatcher takes it until its outcome is
recorded (or its job ends). A 'running' row nobody here owns is an orphan when the startup
recovery has not run yet (it belongs to an earlier process), when this process abandoned it (a
database error left it 'running'), or when it is older than the job timeout plus a grace period
(an engine started by hand gets that long). An orphan is put back to 'pending' (`requeue`, the
one running -> pending move the 025 trigger allows) and dispatched again, by the recovery task
and by an RPC retry alike (R#1, R#2). Terminal rows are final: a retry gets the recorded outcome,
and AlgoLens inserts a new row to try again (contract C4).
"""

from __future__ import annotations

import datetime as dt
import logging
import threading
from dataclasses import dataclass
from typing import Iterable, List, Optional, Set, Tuple

from .command_store import CommandRow, CommandStore
from .config import CommandSettings
from .jobs import Job, JobManager
from .override_mail import ROLES, Sender, run_override_request, send_smtp
from .portfolios import PortfolioConfigError, resolve_portfolio_dir

log = logging.getLogger("desk.commands")

ACCEPTED, DONE, REFUSED, FAILED = "accepted", "done", "refused", "failed"

ENGINE_MODE = {"save": "desk", "override_decision": "override", "publish": "publish"}

# Kinds refused by the agent on a published day (contract C3). A publish row is left to the
# engine, which refuses a second publish but can still complete one whose run already published.
FROZEN_WHEN_PUBLISHED = ("save", "override_request", "override_decision")


@dataclass(frozen=True)
class Outcome:
    status: str  # accepted | done | refused | failed
    message: str


class Dispatcher:
    def __init__(self, store: CommandStore, jobs: JobManager, settings: CommandSettings,
                 sender: Sender = send_smtp):
        self._store = store
        self._jobs = jobs
        self._settings = settings
        self._sender = sender
        self._lock = threading.Lock()
        self._owned: Set[int] = set()
        self._abandoned: Set[int] = set()
        # Decisions are claimed and checked one at a time, so two decisions on one request can
        # never both pass the "no other decision" check (C2).
        self._decision_lock = threading.Lock()
        # Set once the startup recovery has requeued the previous process's running rows.
        self.recovered = False

    # -- ownership -------------------------------------------------------------------------------

    def _take(self, audit_id: int) -> bool:
        with self._lock:
            if audit_id in self._owned:
                return False
            self._owned.add(audit_id)
            return True

    def _release(self, audit_id: int, abandoned: bool = False) -> None:
        with self._lock:
            self._owned.discard(audit_id)
            if abandoned:
                self._abandoned.add(audit_id)
        if abandoned:
            log.warning("row abandoned 'running'; it will be re-driven",
                        extra={"audit_id": audit_id})

    def live_ids(self) -> Set[int]:
        with self._lock:
            return set(self._owned)

    def is_orphan(self, row: CommandRow, age_s: Optional[float],
                  taken_here: bool = False) -> bool:
        """Is this 'running' row safe to requeue (see the module docstring)? `taken_here`: the
        caller has just taken the row itself (dispatch), so ownership says nothing."""
        with self._lock:
            if row.id in self._owned and not taken_here:
                return False
            if row.id in self._abandoned or not self.recovered:
                return True
        return age_s is None or age_s > self._settings.job_timeout_s + self._settings.stale_grace_s

    def requeue_orphans(self, rows: Iterable[Tuple[CommandRow, Optional[float]]],
                        message: str) -> List[int]:
        """Requeue every orphan among `rows` ('running' rows with their ages)."""
        done = []
        for row, age in rows:
            if not self.is_orphan(row, age):
                continue
            if self._store.requeue(row.id, message):
                done.append(row.id)
            with self._lock:
                self._abandoned.discard(row.id)
        return done

    # -- RPC entry points: check the row against the request, then dispatch --------------------

    def run_desk(self, audit_id: int, portfolio_id: str = "", date: Optional[dt.date] = None):
        return self._from_rpc(audit_id, "save", portfolio_id, date)

    def request_override(self, audit_id: int, portfolio_id: str = "",
                         date: Optional[dt.date] = None):
        return self._from_rpc(audit_id, "override_request", portfolio_id, date)

    def record_decision(self, audit_id: int):
        return self._from_rpc(audit_id, "override_decision", "", None)

    def publish(self, audit_id: int, portfolio_id: str, date: dt.date):
        if audit_id == 0:
            row = self._store.newest_pending_publish(portfolio_id, date)
            if row is None:
                return Outcome(REFUSED, f"no pending publish row in trading.position_overrides "
                                        f"for {portfolio_id} on {date}")
            audit_id = row.id
        return self._from_rpc(audit_id, "publish", portfolio_id, date)

    def _from_rpc(self, audit_id, kind, portfolio_id, date) -> Outcome:
        row = self._store.get_row(audit_id)
        if row is None:
            return Outcome(REFUSED, f"no trading.position_overrides row {audit_id}")
        if row.kind != kind:
            return Outcome(REFUSED, f"trading.position_overrides row {audit_id} is a "
                                    f"{row.kind}, not a {kind}")
        if portfolio_id and row.portfolio_id != portfolio_id:
            return Outcome(REFUSED, f"row {audit_id} is for portfolio {row.portfolio_id}, "
                                    f"not {portfolio_id}")
        if date is not None and row.date != date:
            return Outcome(REFUSED, f"row {audit_id} is for {row.date}, not {date}")
        return self.dispatch(row)

    # -- dispatch by kind (RPCs and re-drive) ----------------------------------------------------

    def dispatch(self, row: CommandRow) -> Outcome:
        if not self._take(row.id):  # another thread of this process is on it
            return self._already(row, "running")
        handed_off = False
        claimed: Optional[CommandRow] = None
        try:
            if row.status == "running":
                row = self._redrive_orphan(row)
            if row.status != "pending":
                return self._already(row)
            if row.kind == "override_decision":
                with self._decision_lock:
                    claimed = self._claim(row)
                    if claimed is None:
                        return self._already(self._store.get_row(row.id) or row)
                    out, handed_off = self._decision(claimed)
                    return out
            claimed = self._claim(row)
            if claimed is None:  # another dispatcher got there first
                return self._already(self._store.get_row(row.id) or row)
            out, handed_off = self._run_claimed(claimed)
            return out
        except Exception:
            # A database error after the claim leaves the row 'running' with nobody on it.
            if claimed is not None:
                self._release(row.id, abandoned=True)
                handed_off = True
            raise
        finally:
            if not handed_off:
                self._release(row.id)

    def _claim(self, row: CommandRow) -> Optional[CommandRow]:
        claimed = self._store.claim(row.id)
        if claimed is not None:
            log.info("row claimed", extra={"audit_id": claimed.id, "kind": claimed.kind,
                                           "portfolio_id": claimed.portfolio_id,
                                           "date": str(claimed.date)})
        return claimed

    def _redrive_orphan(self, row: CommandRow) -> CommandRow:
        """A 'running' row asked for again: requeue it if it is an orphan; returns the row as it
        now stands."""
        ages = {r.id: age for r, age in self._store.running_rows()}
        if row.id not in ages:  # it moved on meanwhile
            return self._store.get_row(row.id) or row
        if not self.is_orphan(row, ages[row.id], taken_here=True):
            return row
        if self._store.requeue(row.id, "re-driven: running with no live job"):
            log.warning("orphaned running row requeued on request",
                        extra={"audit_id": row.id, "kind": row.kind})
        with self._lock:
            self._abandoned.discard(row.id)
        return self._store.get_row(row.id) or row

    def _run_claimed(self, claimed: CommandRow) -> Tuple[Outcome, bool]:
        if claimed.kind not in ("save", "override_request", "publish"):
            return self._finish(claimed, REFUSED, None,
                                f"unknown command kind {claimed.kind}"), False
        if claimed.kind == "override_request" and claimed.token_hash:
            # Re-driven after the mail went out (token and result were stored first): never a
            # second token or a second mail (R#4).
            result = claimed.result if isinstance(claimed.result, dict) else {}
            return self._finish(claimed, DONE, result,
                                "already e-mailed; not sent again"), False
        refused = self._published_refusal(claimed)
        if refused:
            return refused, False
        portfolio_dir, refused = self._portfolio_dir(claimed)
        if refused:
            return refused, False
        if claimed.kind == "override_request":
            self._jobs.submit(Job(claimed.portfolio_id, claimed.id, claimed.kind,
                                  lambda: self._override_mail(claimed, portfolio_dir),
                                  self._on_done(claimed.id)))
            return Outcome(ACCEPTED, f"override request {claimed.id} queued"), True
        return self._engine(claimed, portfolio_dir), True

    def _on_done(self, audit_id: int):
        return lambda abandoned: self._release(audit_id, abandoned)

    @staticmethod
    def _already(row: CommandRow, status: Optional[str] = None) -> Outcome:
        message = f"trading.position_overrides row {row.id} is already {status or row.status}"
        if row.message and status is None:
            message += f": {row.message}"
        return Outcome(ACCEPTED, message)

    def _finish(self, row: CommandRow, status: str, result, message: str) -> Outcome:
        self._store.finish(row.id, status, result, message)
        log.info("row finished by the agent", extra={"audit_id": row.id, "kind": row.kind,
                                                     "status": status, "error": message})
        return Outcome(status, message)

    def _published_refusal(self, row: CommandRow) -> Optional[Outcome]:
        if row.kind not in FROZEN_WHEN_PUBLISHED:
            return None
        published = self._store.published_at(row.portfolio_id, row.date)
        if published is None:
            return None
        return self._finish(row, REFUSED, None,
                            f"{row.portfolio_id} {row.date} was published at "
                            f"{published.isoformat()}; a published day is frozen")

    def _portfolio_dir(self, row: CommandRow) -> Tuple[str, Optional[Outcome]]:
        """(dir, None), or ("", the refusal already recorded on the row)."""
        try:
            return resolve_portfolio_dir(self._settings.config_dir, row.portfolio_id), None
        except PortfolioConfigError as exc:
            return "", self._finish(row, REFUSED, None, str(exc))

    def _engine(self, row: CommandRow, portfolio_dir: str) -> Outcome:
        mode = ENGINE_MODE[row.kind]
        self._jobs.submit(self._jobs.engine_job(mode, row.portfolio_id, portfolio_dir,
                                                row.date.isoformat(), row.id, row.kind,
                                                self._on_done(row.id)))
        return Outcome(ACCEPTED, f"{row.kind} {row.id} queued for the engine (--{mode})")

    def _override_mail(self, row: CommandRow, portfolio_dir: str) -> None:
        out = run_override_request(row, portfolio_dir, self._settings, self._store,
                                   self._sender)
        self._finish(row, out.status, out.result, out.message)

    # -- override decision ------------------------------------------------------------------------

    def _decision(self, row: CommandRow) -> Tuple[Outcome, bool]:
        why = self._decision_refusal(row)
        if why:
            return self._finish(row, REFUSED, None, why), False
        if row.payload.get("approved") is False:
            return self._finish(row, DONE, {"approved": False}, "override rejected"), False
        portfolio_dir, refused = self._portfolio_dir(row)
        if refused:
            return refused, False
        return self._engine(row, portfolio_dir), True

    def _decision_refusal(self, row: CommandRow) -> str:
        approved = row.payload.get("approved") if isinstance(row.payload, dict) else None
        if not isinstance(approved, bool):
            return "payload.approved must be true or false"
        if row.parent_id is None:
            return "an override decision needs parent_id (its override request)"
        parent = self._store.get_row(row.parent_id)
        if parent is None or parent.kind != "override_request":
            return f"parent row {row.parent_id} is not an override request"
        if (parent.portfolio_id, parent.date) != (row.portfolio_id, row.date):
            return (f"the decision is for {row.portfolio_id} {row.date} but its request is for "
                    f"{parent.portfolio_id} {parent.date}")
        if (row.approver_role or "") not in ROLES:
            return "approver_role must be vp or president"
        if row.requested_by.strip().lower() == parent.requested_by.strip().lower():
            return "the requester may not approve their own request"
        if parent.token_expires_at is None:
            return f"override request {parent.id} has no approval token (its e-mail was never sent)"
        if parent.status != "done":
            return f"override request {parent.id} is {parent.status}, not done"
        if row.created_at is None or parent.token_expires_at < row.created_at:
            return f"the approval link of override request {parent.id} expired at " \
                   f"{parent.token_expires_at.isoformat()}"
        for other in self._store.decisions_for(parent.id):
            if other.id == row.id:
                continue
            if other.status == "done":
                return (f"override request {parent.id} was already decided "
                        f"(decision {other.id})")
            if other.status == "running":
                return (f"override request {parent.id} is being decided by decision {other.id}")
        published = self._store.published_at(row.portfolio_id, row.date)
        if published is not None:
            return (f"{row.portfolio_id} {row.date} was published at {published.isoformat()}; "
                    "a published day is frozen")
        return ""
