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
| publish           | the desk's approval (contract C7): refused when the day is        |
|                   | published or the row was created at or after 10:00 New York on    |
|                   | its date; else engine `--publish`, with `--send-now` from 09:30.  |
|                   | A scheduler fallback row (requested_by system:fallback-...)       |
|                   | re-driven here runs engine `--fallback` (`--send-now` for         |
|                   | today's 10:00 fallback; a past day is never e-mailed).            |
|                   | A publish whose payload names a `flow` (contract C10) runs after  |
|                   | the row it names (`after`) on the portfolio's queue: `auto` (the  |
|                   | QT's Approve on B) publishes only when the engine's book C equals |
|                   | B and is within the limits, else it is refused with the           |
|                   | differences; `accept_c` publishes C when C is not over a limit;   |
|                   | `override_b` publishes B once the approved override booked it.    |
| rescind           | the desk takes back its approval (contract C8), in Python under   |
|                   | the portfolio's flock: refused from 09:30 New York on its date    |
|                   | (the clock when it runs), on a day not published by the desk, by  |
|                   | anyone but the approver and on a sent day; else                   |
|                   | published_by/at/source are cleared and the                        |
|                   | row is finished done, in one transaction (command_store.rescind). |

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
import os
import threading
from dataclasses import dataclass
from typing import Callable, Iterable, List, Optional, Set, Tuple

from . import cutoff
from .command_store import (NOT_APPROVER, NOT_DESK, NOT_RUNNING, RESCINDED, SENT, UNPUBLISHED,
                            CommandRow, CommandStore)
from .config import CommandSettings
from .jobs import Job, JobManager
from .override_mail import ROLES, Sender, run_override_request, send_smtp
from .portfolios import PortfolioConfigError, resolve_portfolio_dir
from . import risk as rk

log = logging.getLogger("desk.commands")

ACCEPTED, DONE, REFUSED, FAILED = "accepted", "done", "refused", "failed"

ENGINE_MODE = {"save": "desk", "override_decision": "override", "publish": "publish"}

# Kinds refused by the agent on a published day (contract C3). A publish row is checked on its
# own (_publish_plan): a re-run of a row that already approved the day goes on to its send.
FROZEN_WHEN_PUBLISHED = ("save", "override_request", "override_decision")


# Contract C10: the approval flows a publish row's payload may name, and the kind of the row
# each runs after.
FLOWS = {"auto": "save", "accept_c": "save", "override_b": "override_decision"}

# How long a rescind waits for the portfolio's flock (a model run or the 09:30 send may hold it).
RESCIND_LOCK_WAIT_S = 120.0


def flow_of(row: CommandRow) -> str:
    """The C10 approval flow a publish row names ('' for a plain publish)."""
    payload = row.payload if isinstance(row.payload, dict) else {}
    flow = payload.get("flow")
    return flow if isinstance(flow, str) else ""


@dataclass(frozen=True)
class Outcome:
    status: str  # accepted | done | refused | failed
    message: str


class Dispatcher:
    def __init__(self, store: CommandStore, jobs: JobManager, settings: CommandSettings,
                 sender: Sender = send_smtp,
                 clock: Optional[Callable[[], dt.datetime]] = None,
                 lock_factory=None,
                 risk_reader: Optional[Callable[[str, dt.date], Optional[rk.DeskRiskRow]]] = None):
        self._store = store
        # The day's qt row of trading.live_results (store.desk_risk_row): what the C10 approval
        # compares B with and checks the limits on.
        self._risk_reader = risk_reader
        self._jobs = jobs
        self._settings = settings
        self._sender = sender
        self._clock = clock or (lambda: dt.datetime.now(dt.timezone.utc))
        # flock(2) on <lock_dir>/<portfolio_id>.lock, the lock the engine jobs and the catch-up
        # scheduler take (catchup.file_lock); tests inject one.
        self._lock_factory = lock_factory
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

    def hold(self, audit_id: int) -> bool:
        """The catch-up scheduler runs a fallback row itself: while it does, the row is owned
        here, so neither the recovery nor an RPC treats it as an orphan."""
        return self._take(audit_id)

    def release(self, audit_id: int) -> None:
        self._release(audit_id)

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

    def rescind(self, audit_id: int, portfolio_id: str, date: dt.date):
        return self._from_rpc(audit_id, "rescind", portfolio_id, date)

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
        if claimed.kind not in ("save", "override_request", "publish", "rescind"):
            return self._finish(claimed, REFUSED, None,
                                f"unknown command kind {claimed.kind}"), False
        if claimed.kind == "rescind":
            self._jobs.submit(Job(claimed.portfolio_id, claimed.id, claimed.kind,
                                  lambda: self._rescind(claimed), self._on_done(claimed.id)))
            return Outcome(ACCEPTED, f"rescind {claimed.id} queued"), True
        if claimed.kind == "override_request" and claimed.token_hash:
            # Re-driven after the mail went out (token and result were stored first): never a
            # second token or a second mail (R#4).
            result = claimed.result if isinstance(claimed.result, dict) else {}
            return self._finish(claimed, DONE, result,
                                "already e-mailed; not sent again"), False
        refused = self._published_refusal(claimed)
        if refused:
            return refused, False
        mode, send_now = ENGINE_MODE.get(claimed.kind, ""), False
        if claimed.kind == "publish":
            mode, send_now, why = self._publish_plan(claimed)
            if why:
                return self._finish(claimed, REFUSED, None, why), False
            if mode == "publish" and flow_of(claimed):
                return self._queue_flow(claimed)
        portfolio_dir, refused = self._portfolio_dir(claimed)
        if refused:
            return refused, False
        if claimed.kind == "override_request":
            self._jobs.submit(Job(claimed.portfolio_id, claimed.id, claimed.kind,
                                  lambda: self._override_mail(claimed, portfolio_dir),
                                  self._on_done(claimed.id)))
            return Outcome(ACCEPTED, f"override request {claimed.id} queued"), True
        return self._engine(claimed, portfolio_dir, mode, send_now), True

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

    def _engine(self, row: CommandRow, portfolio_dir: str, mode: str = "",
                send_now: bool = False) -> Outcome:
        mode = mode or ENGINE_MODE[row.kind]
        self._jobs.submit(self._jobs.engine_job(mode, row.portfolio_id, portfolio_dir,
                                                row.date.isoformat(), row.id, row.kind,
                                                self._on_done(row.id), send_now=send_now))
        return Outcome(ACCEPTED, f"{row.kind} {row.id} queued for the engine (--{mode}"
                                 f"{' --send-now' if send_now else ''})")

    # -- the approval (contract C7) --------------------------------------------------------------

    def _publish_plan(self, row: CommandRow) -> Tuple[str, bool, str]:
        """(engine mode, --send-now, refusal). The desk's approval: before 09:30 New York it only
        freezes the day (the 09:30 send e-mails it), from 09:30 it also sends; a row created at
        or after 10:00 on its date, or on a published day, is refused. A row that already
        approved (a re-run: its result carries published_at) goes on to its send."""
        now = self._clock()
        if cutoff.is_fallback(row.requested_by):
            today = now.astimezone(cutoff.zone()).date()
            send = row.requested_by == cutoff.FALLBACK_10AM and row.date >= today
            return "fallback", send, ""
        approved_here = isinstance(row.result, dict) and "published_at" in row.result
        if not approved_here:
            published = self._store.published_at(row.portfolio_id, row.date)
            if published is not None:
                return "", False, (f"{row.portfolio_id} {row.date} was already published at "
                                   f"{published.isoformat()}; a published day is frozen")
            created = row.created_at or now
            if cutoff.window(row.date, created) == cutoff.CLOSED:
                return "", False, cutoff.deadline_message(row.date)
        return "publish", cutoff.window(row.date, now) != cutoff.BEFORE_SEND, ""

    # -- the approval flows (contract C10) -----------------------------------------------------

    def _queue_flow(self, row: CommandRow) -> Tuple[Outcome, bool]:
        """Check the row the flow runs after, put it on the portfolio's queue first if it is still
        pending, then queue the flow's job behind it."""
        flow = flow_of(row)
        if flow not in FLOWS:
            return self._finish(row, REFUSED, None,
                                f"unknown approval flow {flow!r} (auto, accept_c or "
                                "override_b)"), False
        after_id = row.payload.get("after")
        after = self._store.get_row(after_id) if isinstance(after_id, int) else None
        if after is None:
            return self._finish(row, REFUSED, None,
                                f"the {flow} approval names no row to run after "
                                f"(payload.after {after_id!r})"), False
        if after.kind != FLOWS[flow] or (after.portfolio_id, after.date) != (row.portfolio_id,
                                                                              row.date):
            return self._finish(row, REFUSED, None,
                                f"the {flow} approval runs after a {FLOWS[flow]} of "
                                f"{row.portfolio_id} {row.date}; row {after.id} is a {after.kind} "
                                f"of {after.portfolio_id} {after.date}"), False
        portfolio_dir, refused = self._portfolio_dir(row)
        if refused:
            return refused, False
        if after.status == "pending":
            # AlgoLens calls RunDesk (or RecordDecision) first; on a re-drive, or a lost call,
            # the row it names goes on the queue ahead of this one.
            self.dispatch(after)
        self._jobs.submit(Job(row.portfolio_id, row.id, row.kind,
                              lambda: self._run_flow(row, flow, after.id, portfolio_dir),
                              self._on_done(row.id)))
        return Outcome(ACCEPTED, f"{flow} approval {row.id} queued after row {after.id}"), True

    def _run_flow(self, row: CommandRow, flow: str, after_id: int, portfolio_dir: str) -> None:
        """On the portfolio's queue, after the row it names: the checks, then the publish."""
        send_now = cutoff.window(row.date, self._clock()) != cutoff.BEFORE_SEND
        if isinstance(row.result, dict) and "published_at" in row.result:
            # A re-run of a row that already approved the day: on to its send.
            self._jobs.run_engine_now("publish", row.portfolio_id, portfolio_dir,
                                      row.date.isoformat(), row.id, send_now)
            return
        refusal = self._flow_refusal(row, flow, after_id)
        if refusal is not None:
            status, result, message = refusal
            self._finish(row, status, result, message)
            return
        self._jobs.run_engine_now("publish", row.portfolio_id, portfolio_dir,
                                  row.date.isoformat(), row.id, send_now)

    def _flow_refusal(self, row: CommandRow, flow: str, after_id: int):
        """(status, result, message), or None when the flow may publish."""
        after = self._store.get_row(after_id)
        if after is None or after.status != DONE:
            what = "dynamic optimisation and the risk check" if flow != "override_b" \
                else "the override"
            return (REFUSED, {"reason": "incomplete", "after": after_id,
                              "after_status": after.status if after else None},
                    f"{what} (row {after_id}) ended "
                    f"{after.status if after else 'missing'}"
                    f"{': ' + after.message if after and after.message else ''}; "
                    "nothing was approved")
        if flow == "override_b":
            approved = isinstance(after.result, dict) and after.result.get("approved") is not False
            if not approved or after.payload.get("approved") is not True:
                return (REFUSED, {"reason": "rejected", "after": after_id},
                        "the override for B was not approved; nothing was published")
            return None
        if self._risk_reader is None:
            return (FAILED, None, "the risk check cannot be read here; try again")
        try:
            report = rk.derive_desk_risk(self._risk_reader(row.portfolio_id, row.date))
        except Exception as exc:  # StoreError and the like: retryable
            return (FAILED, None, f"the risk check could not be read ({type(exc).__name__}); "
                                  "try again")
        if report.audit_id != after_id or report.state != rk.DESK:
            return (REFUSED, {"reason": "stale", "after": after_id,
                              "book_audit_id": report.audit_id},
                    f"the day's book is no longer the result of save {after_id} (it is "
                    f"{report.state} {report.audit_id}); approve the latest save")
        differs = [{"symbol": s.symbol, "asked": s.asked, "given": s.given,
                    "moved_by": s.moved_by, "category": s.category}
                   for s in report.symbols if abs(s.asked - s.given) > 1e-9]
        breaches = [b.term for b in report.breaches]
        if flow == "auto":
            if differs or not report.within_limits:
                return (REFUSED, {"reason": "changed" if differs else "outside_limits",
                                  "after": after_id, "differs": differs,
                                  "within_limits": report.within_limits,
                                  "breaches": breaches},
                        (f"dynamic optimisation and the risk system changed "
                         f"{len(differs)} position(s): approve the engine's book (C) or ask the "
                         "VP or President to approve yours (B)") if differs else
                        ("the positions are outside the risk limits "
                         f"({', '.join(breaches)}): ask the VP or President to approve them"))
            return None
        # C itself over a limit: a check read over, or a term the stored book is still over
        # (after rounding, or through a held position).
        over = [c.term for c in report.checks if c.status == rk.OVER]
        for term in rk.over_terms(report):
            if term not in over:
                over.append(term)
        if over or report.refused:
            return (REFUSED, {"reason": "outside_limits", "after": after_id, "over": over,
                              "refused": report.refused},
                    "the engine's book (C) is itself over a limit "
                    f"({', '.join(over) or 'the overlay refused'}): ask the VP or President")
        return None

    # -- rescind (contract C8) --------------------------------------------------------------------

    def _portfolio_lock(self, portfolio_id: str):
        factory = self._lock_factory
        if factory is None:
            from .catchup import file_lock as factory
        return factory(os.path.join(self._settings.lock_dir, f"{portfolio_id}.lock"),
                       RESCIND_LOCK_WAIT_S)

    def _rescind(self, row: CommandRow) -> None:
        """Under the portfolio's flock, so neither a model run nor the 09:30 send is half-way
        through the day: the time check, then the store's one transaction."""
        with self._portfolio_lock(row.portfolio_id) as held:
            if not held:
                self._finish(row, FAILED, None,
                             f"{row.portfolio_id} is busy (a model run or the send holds its "
                             "lock); nothing changed, try again")
                return
            if cutoff.window(row.date, self._clock()) != cutoff.BEFORE_SEND:
                self._finish(row, REFUSED, None,
                             f"the {row.date} book's {cutoff.SEND_AT:%H:%M} New York send time "
                             "has passed; its approval can no longer be rescinded")
                return
            done = (f"approval of {row.date} rescinded; the day takes edits and a new approval "
                    f"again (by {cutoff.SEND_AT:%H:%M} New York, or the model's book is sent at "
                    f"{cutoff.CUTOFF_AT:%H:%M})")
            res = self._store.rescind(row.id, row.portfolio_id, row.date, done,
                                      row.requested_by)
        if res.outcome == RESCINDED:
            log.info("approval rescinded", extra={
                "audit_id": row.id, "portfolio_id": row.portfolio_id, "date": str(row.date),
                "kind": row.kind, "status": DONE})
            return
        if res.outcome == NOT_RUNNING:
            log.warning("rescind row no longer running; nothing changed",
                        extra={"audit_id": row.id})
            return
        why = {
            UNPUBLISHED: f"{row.portfolio_id} {row.date} is not approved; nothing to rescind",
            NOT_DESK: (f"{row.portfolio_id} {row.date} was published by "
                       f"{res.published_by or 'the engine'} (publish_source "
                       f"{res.publish_source or 'unknown'}), not approved by the desk; only the "
                       "desk's own approval can be rescinded"),
            NOT_APPROVER: (f"{row.portfolio_id} {row.date} was approved by "
                           f"{res.published_by}; only they can rescind their approval"),
            SENT: (f"{row.portfolio_id} {row.date} was already e-mailed at "
                   f"{res.sent_at.isoformat() if res.sent_at else 'an unknown time'}; a sent "
                   "approval cannot be rescinded"),
        }[res.outcome]
        self._finish(row, REFUSED, None, why)

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
