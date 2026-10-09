"""Command dispatch: from a trading.position_overrides row to the work that answers it.

Used by the RPCs (the fast path) and by the re-drive sweep (the path that never loses a row).
Both go through `claim`, which only succeeds on a 'pending' row, so a row runs at most once.

| kind              | work                                                              |
|-------------------|-------------------------------------------------------------------|
| save              | engine `--desk` with the save row's id                            |
| override_request  | the override e-mail, in Python (override_mail.py)                 |
| override_decision | checks here; rejection -> done {"approved": false}; approval ->   |
|                   | engine `--override` with the decision row's id                    |
| publish           | engine `--publish` with the publish row's id                      |
"""

from __future__ import annotations

import datetime as dt
import logging
from dataclasses import dataclass
from typing import Optional, Tuple

from .command_store import CommandRow, CommandStore
from .config import CommandSettings
from .jobs import Job, JobManager
from .override_mail import ROLES, Sender, run_override_request, send_smtp
from .portfolios import PortfolioConfigError, resolve_portfolio_dir

log = logging.getLogger("desk.commands")

ACCEPTED, DONE, REFUSED, FAILED = "accepted", "done", "refused", "failed"

ENGINE_MODE = {"save": "desk", "override_decision": "override", "publish": "publish"}


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
        if row.status != "pending":
            return self._already(row)
        claimed = self._store.claim(row.id)
        if claimed is None:  # another dispatcher got there first
            current = self._store.get_row(row.id) or row
            return self._already(current)
        log.info("row claimed", extra={"audit_id": claimed.id, "kind": claimed.kind,
                                       "portfolio_id": claimed.portfolio_id,
                                       "date": str(claimed.date)})
        if claimed.kind == "override_decision":
            return self._decision(claimed)
        if claimed.kind not in ("save", "override_request", "publish"):
            return self._finish(claimed, REFUSED, None, f"unknown command kind {claimed.kind}")
        portfolio_dir, refused = self._portfolio_dir(claimed)
        if refused:
            return refused
        if claimed.kind == "override_request":
            self._jobs.submit(Job(claimed.portfolio_id, claimed.id, claimed.kind,
                                  lambda: self._override_mail(claimed, portfolio_dir)))
            return Outcome(ACCEPTED, f"override request {claimed.id} queued")
        return self._engine(claimed, portfolio_dir)

    @staticmethod
    def _already(row: CommandRow) -> Outcome:
        message = f"trading.position_overrides row {row.id} is already {row.status}"
        if row.message:
            message += f": {row.message}"
        return Outcome(ACCEPTED, message)

    def _finish(self, row: CommandRow, status: str, result, message: str) -> Outcome:
        self._store.finish(row.id, status, result, message)
        log.info("row finished by the agent", extra={"audit_id": row.id, "kind": row.kind,
                                                     "status": status, "error": message})
        return Outcome(status, message)

    def _portfolio_dir(self, row: CommandRow) -> Tuple[str, Optional[Outcome]]:
        """(dir, None), or ("", the refusal already recorded on the row)."""
        try:
            return resolve_portfolio_dir(self._settings.config_dir, row.portfolio_id), None
        except PortfolioConfigError as exc:
            return "", self._finish(row, REFUSED, None, str(exc))

    def _engine(self, row: CommandRow, portfolio_dir: str) -> Outcome:
        mode = ENGINE_MODE[row.kind]
        self._jobs.submit(self._jobs.engine_job(mode, row.portfolio_id, portfolio_dir,
                                                row.date.isoformat(), row.id, row.kind))
        return Outcome(ACCEPTED, f"{row.kind} {row.id} queued for the engine (--{mode})")

    def _override_mail(self, row: CommandRow, portfolio_dir: str) -> None:
        out = run_override_request(row, portfolio_dir, self._settings, self._store,
                                   self._sender)
        self._finish(row, out.status, out.result, out.message)

    # -- override decision ------------------------------------------------------------------------

    def _decision(self, row: CommandRow) -> Outcome:
        why = self._decision_refusal(row)
        if why:
            return self._finish(row, REFUSED, None, why)
        if row.payload.get("approved") is False:
            return self._finish(row, DONE, {"approved": False}, "override rejected")
        portfolio_dir, refused = self._portfolio_dir(row)
        if refused:
            return refused
        return self._engine(row, portfolio_dir)

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
        if row.created_at is None or parent.token_expires_at < row.created_at:
            return f"the approval link of override request {parent.id} expired at " \
                   f"{parent.token_expires_at.isoformat()}"
        return ""
