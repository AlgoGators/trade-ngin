"""The QT scheduler (contract C6, C7; rulings 18 and 29 as amended 2026-10-09), in the rpc host.

A background task of the desk service (`desk.catchup`, service.py) ticks every minute. A pass is
due once per slot inside QT_CATCHUP_WINDOW (06:30-22:00 America/New_York), every calendar day:
every QT_CATCHUP_BUSY_EVERY_MIN (5) minutes inside QT_CATCHUP_BUSY_WINDOW (06:30-10:30), every
QT_CATCHUP_EVERY_MIN (30) after. A pass holds <QT_LOCK_DIR>/qt-catchup.lock (so the host
watchdog's pass and the in-process one never overlap) and, for each portfolio of
QT_CATCHUP_PORTFOLIOS, under that portfolio's flock, takes one step at a time until it has
nothing to do. Every step is decided from the database and the clock, so a pass that runs late
(a restart, a missed tick: 10:07) does what the missed one would have.

A desk-editable portfolio (its day D: model run ~07:00, approve by 09:30, fallback at 10:00):

    last = the newest qt day (none yet: the newest system day; ruling 27)
    last unpublished, last < today      -> FALLBACK last: catch-up of a missed past day
                                           (system:fallback-catchup; qt = the model's book; not
                                           e-mailed; one alert)
    last unpublished, today, >= 10:00   -> FALLBACK today: qt = the model's book, published
                                           (system:fallback-10am) and e-mailed at once; an alert
                                           to the President
    last < today                        -> RUN the model for last + 1 (today's not before 06:45,
                                           retried every QT_MODEL_RETRY_MIN after a failure)
    today approved, not sent, >= 09:30  -> SEND today (engine --send: from the stored rows, once)
    otherwise                           -> wait for the desk's approval, or up to date

The model twin (desk editing off) runs the same model runs; each day it writes is published at
once as system:model-only (publish_source 'model-only') and, today, sent at 09:30 when its
email.json names recipients. At QT_MODEL_ALERT_AT (08:30) a portfolio with no model book for
today raises an alert. The 24 h unpublished reminder is gone: the 10:00 fallback publishes every
day. A failure or refusal (anything the engine declines) alerts (ruling 29), and the step is
retried on a later pass.

Each model run is exactly what scripts/qt_model_run.sh runs: `<QT_ENGINE_BINARY>
--portfolio-config <dir> --date <day>` in QT_ENGINE_CWD; the fallback is `--fallback ...
--audit-id <its publish row> [--send-now]`, the send `--send --portfolio-config <dir> --date
<day>`. The scheduler holds <QT_LOCK_DIR>/<portfolio_id>.lock (flock(2), the lock flock(1) takes
for desk jobs) around each step and re-reads the database under it, so nothing runs twice.
QT_JOB_TIMEOUT_S bounds every run.

Alerts: one line in <QT_LOG_DIR>/qt-catchup.log and an e-mail to the president address of
QT_APPROVERS through the portfolio's email.json (not when QT_EMAIL_DISABLED=1), at most once per
(portfolio, date, reason) per New York day (state in <QT_LOG_DIR>/qt-alerts.json).

    python -m algogators_rpc.services.desk.catchup run-once   # one pass now (any hour)
    python -m algogators_rpc.services.desk.catchup watchdog   # a pass only if none ran lately
"""

from __future__ import annotations

import contextlib
import datetime as dt
import json
import logging
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import (Any, Callable, Dict, Iterator, List, Optional, Protocol, Sequence, Tuple)

from . import cutoff
from .config import (CONNECT_TIMEOUT_S, CatchupSettings, CommandSettings, ConfigError, DbConfig,
                     load_settings)
from .jobs import Runner, RunResult, SubprocessRunner, tail
from .override_mail import ApproverError, Sender, load_smtp, parse_approvers, send_smtp
from .portfolios import portfolios_root

log = logging.getLogger("desk.catchup")

UTC = dt.timezone.utc
CATCHUP_LOG = "qt-catchup.log"
ENGINE_LOG = "qt-engine-runs.log"
ALERT_STATE = "qt-alerts.json"
HEARTBEAT = "qt-catchup.heartbeat"
SCHEDULER_LOCK = "qt-catchup.lock"
# A pass never takes more than this many steps per portfolio (a sanity bound, not a policy).
MAX_DAYS_PER_PASS = 62


def _zone(name: str):
    from zoneinfo import ZoneInfo

    return ZoneInfo(name)


# -- the database -------------------------------------------------------------------------------

class DayStore(Protocol):
    def last_book_date(self, portfolio_id: str, book: str) -> Optional[dt.date]: ...

    def published_at(self, portfolio_id: str, date: dt.date) -> Optional[dt.datetime]: ...

    def sent_at(self, portfolio_id: str, date: dt.date) -> Optional[dt.datetime]: ...

    def book_written_at(self, portfolio_id: str, date: dt.date,
                        book: str) -> Optional[dt.datetime]: ...

    def publish_model_only(self, portfolio_id: str, date: dt.date) -> bool:
        """The model twin's day: published_by system:model-only, publish_source 'model-only',
        published_at now(), while unpublished. True when it published."""

    def insert_fallback(self, portfolio_id: str, date: dt.date, requested_by: str) -> Optional[int]:
        """A 'publish' row for the fallback, inserted pending and moved to running in one
        transaction (migration 025's rules). None when another publish row of the day is open."""

    def command_outcome(self, audit_id: int) -> Tuple[str, Dict[str, Any], str]:
        """(status, result, message) of a command row."""

    def fail_running(self, audit_id: int, message: str) -> bool: ...


class PostgresDayStore:
    def __init__(self, db: DbConfig, connect_timeout: int = CONNECT_TIMEOUT_S):
        self._db = db
        self._connect_timeout = connect_timeout

    def _run(self, fn, autocommit: bool = True):
        import psycopg

        from .store import StoreError
        try:
            with psycopg.connect(**self._db.conninfo_kwargs(self._connect_timeout),
                                 autocommit=autocommit) as conn, conn.cursor() as cur:
                return fn(cur)
        except Exception as exc:
            raise StoreError(f"database error ({type(exc).__name__})") from exc

    def _one(self, sql: str, args):
        def q(cur):
            cur.execute(sql, args)
            rec = cur.fetchone()
            return rec[0] if rec else None
        return self._run(q)

    def last_book_date(self, portfolio_id, book):
        return self._one("SELECT max(date) FROM trading.live_results "
                         "WHERE portfolio_id = %s AND portfolio_type = %s", (portfolio_id, book))

    def published_at(self, portfolio_id, date):
        return self._one("SELECT max(published_at) FROM trading.live_run_metadata "
                         "WHERE portfolio_id = %s AND date = %s", (portfolio_id, date))

    def sent_at(self, portfolio_id, date):
        return self._one("SELECT max(sent_at) FROM trading.live_run_metadata "
                         "WHERE portfolio_id = %s AND date = %s", (portfolio_id, date))

    def book_written_at(self, portfolio_id, date, book):
        # created_at is timestamp without time zone in the writer's session zone (R12).
        return self._one("SELECT max(created_at)::timestamptz FROM trading.live_results "
                         "WHERE portfolio_id = %s AND date = %s AND portfolio_type = %s",
                         (portfolio_id, date, book))

    def publish_model_only(self, portfolio_id, date):
        def q(cur):
            cur.execute("UPDATE trading.live_run_metadata SET published_by = %s, "
                        "published_at = now(), publish_source = 'model-only' "
                        "WHERE portfolio_id = %s AND date = %s AND published_at IS NULL "
                        "RETURNING id", (cutoff.MODEL_ONLY, portfolio_id, date))
            return cur.fetchone() is not None
        return self._run(q)

    def insert_fallback(self, portfolio_id, date, requested_by):
        import psycopg

        def q(cur):
            try:
                cur.execute("INSERT INTO trading.position_overrides "
                            "(portfolio_id, date, kind, requested_by) "
                            "VALUES (%s, %s, 'publish', %s) RETURNING id",
                            (portfolio_id, date, requested_by))
            except psycopg.errors.UniqueViolation:
                cur.connection.rollback()  # migration 025: another publish row is open
                return None
            audit_id = cur.fetchone()[0]
            cur.execute("UPDATE trading.position_overrides SET status = 'running', "
                        "started_at = now() WHERE id = %s AND status = 'pending'", (audit_id,))
            return audit_id
        return self._run(q, autocommit=False)

    def command_outcome(self, audit_id):
        def q(cur):
            cur.execute("SELECT status, result, message FROM trading.position_overrides "
                        "WHERE id = %s", (audit_id,))
            rec = cur.fetchone()
            if rec is None:
                return ("missing", {}, "")
            result = rec[1] if isinstance(rec[1], dict) else {}
            return (rec[0], result, rec[2] or "")
        return self._run(q)

    def fail_running(self, audit_id, message):
        def q(cur):
            cur.execute("UPDATE trading.position_overrides SET status = 'failed', message = %s, "
                        "finished_at = now() WHERE id = %s AND status = 'running' RETURNING id",
                        (message, audit_id))
            return cur.fetchone() is not None
        return self._run(q)


# -- logs, locks, alerts ----------------------------------------------------------------------

class FileLog:
    """Append-only text logs in QT_LOG_DIR. Each write opens and closes the file, so logrotate
    can rename it at any time (deploy/qt-engine.logrotate)."""

    def __init__(self, log_dir: str, clock: Callable[[], dt.datetime]):
        self.dir = Path(log_dir)
        self._clock = clock

    def path(self, name: str) -> Path:
        return self.dir / name

    def line(self, text: str, name: str = CATCHUP_LOG) -> None:
        stamp = self._clock().astimezone(UTC).isoformat(timespec="seconds")
        try:
            self.dir.mkdir(parents=True, exist_ok=True)
            with open(self.dir / name, "a", encoding="utf-8") as f:
                f.write(f"{stamp} {text}\n")
        except OSError as exc:
            log.error("cannot write the catch-up log", extra={"error": type(exc).__name__})

    @contextlib.contextmanager
    def append(self, name: str) -> Iterator[Optional[object]]:
        try:
            self.dir.mkdir(parents=True, exist_ok=True)
            f = open(self.dir / name, "a", encoding="utf-8", errors="replace")
        except OSError:
            yield None
            return
        with f:
            yield f


@contextlib.contextmanager
def file_lock(path: str, wait_s: float = 0.0, poll_s: float = 1.0) -> Iterator[bool]:
    """flock(2) on `path` (the lock flock(1) takes). Yields True when held, False when another
    holder kept it for `wait_s`."""
    import fcntl

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    held = False
    try:
        deadline = time.monotonic() + wait_s
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                held = True
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    break
                time.sleep(poll_s)
        yield held
    finally:
        if held:
            fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


LockFactory = Callable[[str, float], "contextlib.AbstractContextManager[bool]"]


class Alerter:
    """At most one alert per (portfolio, date, reason) per New York day: a line in the catch-up
    log and an e-mail to the president of QT_APPROVERS."""

    def __init__(self, settings: CommandSettings, files: FileLog, sender: Sender,
                 clock: Callable[[], dt.datetime], tz):
        self._settings = settings
        self._files = files
        self._sender = sender
        self._clock = clock
        self._tz = tz

    def _state_path(self) -> Path:
        return self._files.path(ALERT_STATE)

    def _load(self) -> Dict[str, str]:
        try:
            data = json.loads(self._state_path().read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def _save(self, state: Dict[str, str], today: dt.date) -> None:
        keep = (today - dt.timedelta(days=30)).isoformat()
        state = {k: v for k, v in state.items() if isinstance(v, str) and v >= keep}
        try:
            self._files.dir.mkdir(parents=True, exist_ok=True)
            tmp = self._state_path().with_suffix(".tmp")
            tmp.write_text(json.dumps(state, sort_keys=True), encoding="utf-8")
            os.replace(tmp, self._state_path())
        except OSError as exc:
            log.error("cannot write the alert state", extra={"error": type(exc).__name__})

    def alert(self, portfolio_dir: str, portfolio_id: str, date: Optional[dt.date], reason: str,
              message: str, subject: Optional[str] = None) -> bool:
        """Returns True when this call raised the alert (False: already raised today)."""
        today = self._clock().astimezone(self._tz).date()
        key = f"{portfolio_id}|{date or '-'}|{reason}"
        state = self._load()
        if state.get(key) == today.isoformat():
            log.info("alert already raised today", extra={"portfolio_id": portfolio_id,
                                                         "date": str(date), "reason": reason})
            return False
        self._files.line(f"ALERT {portfolio_id} {date or '-'} {reason}: {message}")
        log.warning("catch-up alert", extra={"portfolio_id": portfolio_id, "date": str(date),
                                             "reason": reason, "error": message})
        mailed, why = self._mail(portfolio_dir, portfolio_id, date, reason, message, subject)
        if mailed:
            self._files.line(f"ALERT {portfolio_id} {date or '-'} {reason}: e-mailed to the "
                             "president")
        else:
            self._files.line(f"ALERT {portfolio_id} {date or '-'} {reason}: not e-mailed ({why})")
            if why.startswith("send failed"):
                return True  # not recorded: the next pass tries the e-mail again
        state[key] = today.isoformat()
        self._save(state, today)
        return True

    def _mail(self, portfolio_dir, portfolio_id, date, reason, message,
              subject) -> Tuple[bool, str]:
        if self._settings.email_disabled:
            return False, "QT_EMAIL_DISABLED=1"
        try:
            president = parse_approvers(self._settings.approvers)["president"]
        except ApproverError as exc:
            return False, str(exc)
        smtp, why = load_smtp(self._settings, portfolio_dir)
        if smtp is None:
            return False, why
        import html

        subject = subject or f"QT alert: {portfolio_id} {date or ''} {reason}".strip()
        body = ("<html><body>"
                f"<p><b>{html.escape(portfolio_id)}</b>, book date "
                f"<b>{html.escape(str(date or '-'))}</b>: {html.escape(reason)}</p>"
                f"<pre>{html.escape(message)}</pre>"
                "<p>From the QT scheduler on engine-rpc. Details: "
                "/home/ubuntu/qt-engine/logs/qt-catchup.log and qt-engine-runs.log. This alert "
                "is sent at most once a day per portfolio, date and reason.</p>"
                "</body></html>")
        try:
            self._sender(smtp, [president], subject, body)
        except Exception as exc:
            return False, f"send failed: {type(exc).__name__}"
        return True, ""


# -- the scheduler ------------------------------------------------------------------------------

(RUN, FALLBACK, SEND, PUBLISH_MODEL, WAIT_PUBLISH, WAIT_TIME, UP_TO_DATE, ALERT) = (
    "run", "fallback", "send", "publish_model", "wait_publish", "wait_time", "up_to_date",
    "alert")
ACTIONS = (RUN, FALLBACK, SEND, PUBLISH_MODEL)


@dataclass(frozen=True)
class Step:
    action: str
    date: Optional[dt.date] = None
    reason: str = ""
    message: str = ""


@dataclass(frozen=True)
class PortfolioInfo:
    dir: str
    portfolio_id: str
    desk_editable: bool

    @property
    def book(self) -> str:
        """The book the day goes out from: qt on a desk-editable portfolio, else system."""
        return "qt" if self.desk_editable else "system"


def read_portfolio(config_dir: str, portfolio_dir: str) -> PortfolioInfo:
    path = portfolios_root(config_dir) / portfolio_dir / "portfolio.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ConfigError(f"cannot read {path}: {type(exc).__name__}") from None
    pid = data.get("portfolio_id") if isinstance(data, dict) else None
    if not isinstance(pid, str) or not pid:
        raise ConfigError(f"{path} has no portfolio_id")
    qt = data.get("qt") if isinstance(data.get("qt"), dict) else {}
    return PortfolioInfo(portfolio_dir, pid, qt.get("desk_editable") is True)


def recipients(config_dir: str, portfolio_dir: str) -> List[str]:
    """The daily e-mail's recipients: to_emails of the portfolio's email.json ([] when none)."""
    path = portfolios_root(config_dir) / portfolio_dir / "email.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    to = data.get("to_emails") if isinstance(data, dict) else None
    return [t for t in to if isinstance(t, str) and t.strip()] if isinstance(to, list) else []


class CatchupScheduler:
    def __init__(self, store: DayStore, settings: CommandSettings, catchup: CatchupSettings,
                 runner: Optional[Runner] = None, sender: Sender = send_smtp,
                 clock: Optional[Callable[[], dt.datetime]] = None,
                 lock: Optional[LockFactory] = None, dispatcher=None):
        self._store = store
        self._settings = settings
        self._catchup = catchup
        self._runner = runner or SubprocessRunner()
        self._clock = clock or (lambda: dt.datetime.now(UTC))
        self._tz = _zone(catchup.timezone)
        self._lock = lock or file_lock
        # The desk service's dispatcher, when the scheduler runs in the server: a fallback row is
        # held there while the scheduler runs it, so the recovery never takes it for an orphan.
        self._dispatcher = dispatcher
        self.files = FileLog(settings.log_dir, self._clock)
        self.alerter = Alerter(settings, self.files, sender, self._clock, self._tz)
        self._last_slot: Optional[Tuple[dt.date, int, int]] = None
        # (portfolio_id, date, action) -> not before (UTC): a failed run waits before its retry.
        self._retry_after: Dict[Tuple[str, dt.date, str], dt.datetime] = {}

    # -- when -----------------------------------------------------------------------------------

    def local_now(self) -> dt.datetime:
        return self._clock().astimezone(self._tz)

    def in_window(self, now_local: dt.datetime) -> bool:
        t = now_local.time()
        return self._catchup.window_start <= t <= self._catchup.window_end

    def in_busy_window(self, now_local: dt.datetime) -> bool:
        t = now_local.time()
        return self._catchup.busy_start <= t < self._catchup.busy_end

    def every_minutes(self, now_local: dt.datetime) -> int:
        return (self._catchup.busy_every_minutes if self.in_busy_window(now_local)
                else self._catchup.every_minutes)

    def _slot(self, now_local: dt.datetime) -> Tuple[dt.date, int, int]:
        every = self.every_minutes(now_local)
        return (now_local.date(), now_local.hour, now_local.minute // every * every)

    def tick(self) -> bool:
        """The background task: run a pass when one is due. Returns True if a pass ran."""
        if not self._catchup.enabled:
            return False
        now_local = self.local_now()
        if not self.in_window(now_local):
            return False
        slot = self._slot(now_local)
        if slot == self._last_slot:
            return False
        self._last_slot = slot
        self.run_pass()
        return True

    def heartbeat_age_s(self) -> Optional[float]:
        try:
            stamp = dt.datetime.fromisoformat(
                self.files.path(HEARTBEAT).read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            return None
        return (self._clock() - stamp).total_seconds()

    def watchdog_limit_s(self, now_local: dt.datetime) -> float:
        """How old the last pass may be before the watchdog runs one: two slots and a margin
        (5 min in the busy window, so the 09:30 send and the 10:00 fallback are at most ~15 min
        late when the in-process scheduler is down)."""
        every = self.every_minutes(now_local)
        return 2 * every * 60 + (300 if self.in_busy_window(now_local) else 900)

    # -- a pass ---------------------------------------------------------------------------------

    def run_pass(self) -> Optional[List[Tuple[str, Step]]]:
        """One pass over every portfolio. None when another pass holds the scheduler lock."""
        lock_path = os.path.join(self._settings.lock_dir, SCHEDULER_LOCK)
        with self._lock(lock_path, 0.0) as held:
            if not held:
                log.info("catch-up pass skipped: another pass holds the lock")
                return None
            self.files.line("pass started")
            outcomes = []
            for pdir in self._catchup.portfolios:
                try:
                    outcomes.append((pdir, self.catch_up(pdir)))
                except Exception as exc:  # one portfolio never stops the others
                    log.error("catch-up of a portfolio failed", exc_info=True,
                              extra={"portfolio_id": pdir, "error": type(exc).__name__})
                    self.alerter.alert(pdir, pdir, None, "scheduler_error",
                                       f"the catch-up pass failed: {type(exc).__name__}: {exc}")
                    outcomes.append((pdir, Step(ALERT, reason="scheduler_error")))
            self.files.line("pass done: " + ", ".join(
                f"{p}={s.action}{'@' + str(s.date) if s.date else ''}" for p, s in outcomes))
            try:
                self.files.dir.mkdir(parents=True, exist_ok=True)
                self.files.path(HEARTBEAT).write_text(self._clock().isoformat(),
                                                      encoding="utf-8")
            except OSError:
                pass
            return outcomes

    def catch_up(self, portfolio_dir: str) -> Step:
        """Step one portfolio forward until it has nothing to do. Returns the last step."""
        try:
            info = read_portfolio(self._settings.config_dir, portfolio_dir)
        except ConfigError as exc:
            self.alerter.alert(portfolio_dir, portfolio_dir, None, "config", str(exc))
            return Step(ALERT, reason="config", message=str(exc))
        lock_path = os.path.join(self._settings.lock_dir, f"{info.portfolio_id}.lock")
        step = Step(UP_TO_DATE)
        for _ in range(MAX_DAYS_PER_PASS):
            with self._lock(lock_path, self._catchup.lock_wait_s) as held:
                if not held:
                    self.files.line(f"{info.portfolio_id}: busy (its lock is held); next pass")
                    return Step(WAIT_TIME, reason="busy")
                step = self.next_step(info)
                if step.action not in ACTIONS:
                    break
                result_step = self._act(info, step)
            if result_step is not None:
                step = result_step
                break
        self._model_book_check(info)
        self._report(info, step)
        return step

    def _blocked(self, pid: str, day: dt.date, action: str) -> Optional[Step]:
        until = self._retry_after.get((pid, day, action))
        if until is not None and self._clock() < until:
            return Step(WAIT_TIME, day, "retry",
                        f"{action} of {day} failed; retried after "
                        f"{until.astimezone(self._tz):%H:%M}")
        return None

    def _failed(self, pid: str, day: dt.date, action: str) -> None:
        self._retry_after[(pid, day, action)] = (
            self._clock() + dt.timedelta(minutes=self._catchup.model_retry_minutes))

    def next_step(self, info: PortfolioInfo) -> Step:
        now_local = self.local_now()
        today = now_local.date()
        pid = info.portfolio_id
        last = self._store.last_book_date(pid, info.book)
        from_system = False
        if last is None and info.desk_editable:
            last = self._store.last_book_date(pid, "system")  # the first desk day (ruling 27)
            from_system = last is not None
        if last is not None and last > today:
            return Step(UP_TO_DATE, last)
        if info.desk_editable and last is not None and not from_system \
                and self._store.published_at(pid, last) is None:
            if last < today:
                return self._blocked(pid, last, FALLBACK) or Step(
                    FALLBACK, last, "caught_up", f"{last} was never approved")
            if now_local >= cutoff.cutoff_time(today):
                return self._blocked(pid, last, FALLBACK) or Step(
                    FALLBACK, last, "fallback", f"{last} was not approved by 10:00")
            return Step(WAIT_PUBLISH, last, "unpublished",
                        f"{last} waits for the desk's approval (by 09:30; the 10:00 fallback "
                        "sends the model's book)")
        if last is None or last < today or from_system:
            # (from_system and last == today: today's model run wrote system but no qt; again)
            day = today if last is None or last >= today else last + dt.timedelta(days=1)
            if day == today and now_local.time() < self._catchup.today_not_before:
                return Step(WAIT_TIME, day, "before_data",
                            f"today's run waits until {self._catchup.today_not_before:%H:%M}")
            return self._blocked(pid, day, RUN) or Step(RUN, day)
        # today has its book
        if not info.desk_editable and self._store.published_at(pid, today) is None:
            return Step(PUBLISH_MODEL, today)
        published = self._store.published_at(pid, today)
        if published is not None and now_local >= cutoff.send_time(today) \
                and self._store.sent_at(pid, today) is None:
            why = self._not_sendable(info)
            if why:
                return Step(UP_TO_DATE, today, "not_sent", why)
            return self._blocked(pid, today, SEND) or Step(SEND, today)
        return Step(UP_TO_DATE, today)

    def _not_sendable(self, info: PortfolioInfo) -> str:
        if self._settings.email_disabled:
            return "QT_EMAIL_DISABLED=1: the daily e-mail is not sent"
        if not recipients(self._settings.config_dir, info.dir):
            return "its email.json names no recipients (to_emails): not sent"
        smtp, why = load_smtp(self._settings, info.dir)
        if smtp is None:
            return f"the daily e-mail cannot be sent: {why}"
        return ""

    # -- the steps ------------------------------------------------------------------------------

    def _act(self, info: PortfolioInfo, step: Step) -> Optional[Step]:
        """Run one step (the portfolio lock is held). None when it did its work, else the step
        to stop on (an alert or a wait)."""
        if step.action == RUN:
            return self._run_model(info, step.date)
        if step.action == FALLBACK:
            return self._fallback(info, step.date, send=step.reason == "fallback")
        if step.action == SEND:
            return self._send(info, step.date)
        if self._store.publish_model_only(info.portfolio_id, step.date):
            self.files.line(f"{info.portfolio_id}: {step.date} published "
                            f"({cutoff.MODEL_ONLY})")
        return None

    def _engine(self, info: PortfolioInfo, argv: List[str], what: str,
                day: dt.date) -> Tuple[RunResult, str, float]:
        s = self._settings
        self.files.line(f"{info.portfolio_id}: {what} {day} ({' '.join(argv)})")
        started = time.monotonic()
        with self.files.append(ENGINE_LOG) as f:
            if f is not None:
                f.write(f"=== {self._clock().isoformat(timespec='seconds')} {info.portfolio_id} "
                        f"({info.dir}) {what} {day}\n")
            res = self._runner.run(argv, s.engine_cwd, s.job_timeout_s, copy_to=f)
            if f is not None:
                f.write(f"=== exit {res.rc}{' (timed out)' if res.timed_out else ''}\n")
        err = tail(res.stderr, 5) or tail(res.stdout, 5)
        elapsed = round(time.monotonic() - started, 1)
        log.info("catch-up engine run", extra={"portfolio_id": info.portfolio_id,
                                               "mode": what, "date": str(day), "rc": res.rc,
                                               "timed_out": res.timed_out, "error": res.error,
                                               "elapsed_ms": elapsed * 1000,
                                               "stderr_tail": err})
        self.files.line(f"{info.portfolio_id}: {what} {day} exit {res.rc} in {elapsed}s")
        return res, "\n".join(err), elapsed

    def _run_model(self, info: PortfolioInfo, day: dt.date) -> Optional[Step]:
        s = self._settings
        argv = [s.engine_binary, "--portfolio-config", info.dir, "--date", day.isoformat()]
        res, detail, _ = self._engine(info, argv, "model run", day)
        if res.error is not None:
            self._failed(info.portfolio_id, day, RUN)
            return Step(ALERT, day, "engine_not_started", res.error)
        if res.timed_out:
            self._failed(info.portfolio_id, day, RUN)
            return Step(ALERT, day, "timeout",
                        f"the model run was killed after {s.job_timeout_s:g}s\n{detail}")
        if res.rc != 0:
            self._failed(info.portfolio_id, day, RUN)
            reason = "refused" if res.rc == 2 or "QT_REFUSED" in detail or \
                "QT_ALERT" in detail else "failed"
            return Step(ALERT, day, reason, f"the model run exited {res.rc}\n{detail}")
        latest = self._store.last_book_date(info.portfolio_id, info.book)
        if latest is None or latest < day:
            self._failed(info.portfolio_id, day, RUN)
            return Step(ALERT, day, "no_book",
                        f"the model run exited 0 but wrote no {info.book} book for {day}\n"
                        f"{detail}")
        if not info.desk_editable and self._store.publish_model_only(info.portfolio_id, day):
            self.files.line(f"{info.portfolio_id}: {day} published ({cutoff.MODEL_ONLY})")
        return None

    def _fallback(self, info: PortfolioInfo, day: dt.date, send: bool) -> Optional[Step]:
        """Contract C7: qt reset to the model's book and published by the engine (--fallback),
        e-mailed at once for today (10:00) and never for a past day (the catch-up)."""
        pid = info.portfolio_id
        requested_by = cutoff.FALLBACK_10AM if send else cutoff.FALLBACK_CATCHUP
        audit_id = self._store.insert_fallback(pid, day, requested_by)
        if audit_id is None:
            return Step(WAIT_TIME, day, "publish_open",
                        f"{day} has an open publish row (an approval is being run); the next "
                        "pass looks again")
        held = self._dispatcher.hold(audit_id) if self._dispatcher is not None else False
        try:
            argv = [self._settings.engine_binary, "--fallback", "--portfolio-config", info.dir,
                    "--date", day.isoformat(), "--audit-id", str(audit_id)]
            if send:
                argv.append("--send-now")
            res, detail, _ = self._engine(info, argv, "fallback", day)
            status, result, message = self._store.command_outcome(audit_id)
            if status == "running":
                why = (res.error and f"cannot start the engine: {res.error}") or (
                    res.timed_out and f"engine timed out after {self._settings.job_timeout_s:g}s")\
                    or f"engine exited {res.rc} without recording an outcome: " \
                       f"{detail.splitlines()[-1] if detail else '(no output)'}"
                self._store.fail_running(audit_id, why)
                status, message = "failed", why
        finally:
            if held:
                self._dispatcher.release(audit_id)
        when = self.local_now()
        if status == "refused" and "already published" in message:
            self.files.line(f"{pid}: fallback of {day} not needed: {message}")
            return None
        if status != "done":
            self._failed(pid, day, FALLBACK)
            return Step(ALERT, day, f"fallback_{status}",
                        f"the fallback (publish row {audit_id}) is {status}: {message}\n{detail}")
        if not send:
            self.alerter.alert(info.dir, pid, day, "caught_up",
                               f"{pid} {day} was not approved, and its day had passed: it was "
                               f"caught up with the model's book ({cutoff.FALLBACK_CATCHUP}, "
                               f"publish row {audit_id}) and not e-mailed")
            return None
        if result.get("emailed"):
            text = (f"QT did not approve {pid} {day}; the model's book was sent at "
                    f"{when:%H:%M}")
        elif result.get("email_disabled"):
            text = (f"QT did not approve {pid} {day}; the model's book was published at "
                    f"{when:%H:%M} (e-mail disabled on the engine: not sent)")
        else:
            self._failed(pid, day, SEND)
            text = (f"QT did not approve {pid} {day}; the model's book was published at "
                    f"{when:%H:%M}, but the e-mail could not be sent ({message}); the scheduler "
                    "retries the send")
        self.alerter.alert(info.dir, pid, day, "fallback", text + f" (publish row {audit_id})",
                           subject=text)
        return None

    def _send(self, info: PortfolioInfo, day: dt.date) -> Optional[Step]:
        argv = [self._settings.engine_binary, "--send", "--portfolio-config", info.dir,
                "--date", day.isoformat()]
        res, detail, _ = self._engine(info, argv, "send", day)
        if res.rc == 0 and self._store.sent_at(info.portfolio_id, day) is not None:
            self.files.line(f"{info.portfolio_id}: {day} e-mailed")
            return None
        self._failed(info.portfolio_id, day, SEND)
        why = (res.error and f"cannot start the engine: {res.error}") or (
            res.timed_out and "the send timed out") or (
            f"the send exited {res.rc}" + (" without recording sent_at" if res.rc == 0 else ""))
        return Step(ALERT, day, "send_failed", f"{why}\n{detail}")

    def _model_book_check(self, info: PortfolioInfo) -> None:
        """08:30: today has no model book yet -> alert the President."""
        now_local = self.local_now()
        if now_local.time() < self._catchup.model_alert_at:
            return
        today = now_local.date()
        if self._store.book_written_at(info.portfolio_id, today, info.book) is not None:
            return
        self.alerter.alert(info.dir, info.portfolio_id, today, "model_not_done",
                           f"model run not done: {info.portfolio_id} has no model book for "
                           f"{today} at {now_local:%H:%M} New York (the desk cannot approve "
                           "it, and the 10:00 fallback has nothing to send)")

    def _report(self, info: PortfolioInfo, step: Step) -> None:
        pid = info.portfolio_id
        if step.action == ALERT:
            self.alerter.alert(info.dir, pid, step.date, step.reason, step.message)
        elif step.action in (WAIT_PUBLISH, WAIT_TIME):
            self.files.line(f"{pid}: {step.message or step.reason}")
        elif step.reason == "not_sent":
            self.files.line(f"{pid}: {step.date}: {step.message}")
        else:
            self.files.line(f"{pid}: up to date ({step.date})")


# -- CLI (the host watchdog) --------------------------------------------------------------------

def build_scheduler(env=None) -> CatchupScheduler:
    settings = load_settings(env)
    return CatchupScheduler(PostgresDayStore(settings.db), settings.commands, settings.catchup)


def main(argv: Optional[Sequence[str]] = None, scheduler: Optional[CatchupScheduler] = None,
         env=None) -> int:
    from ...log import setup

    argv = list(sys.argv[1:] if argv is None else argv)
    if argv not in (["run-once"], ["watchdog"]):
        print("usage: python -m algogators_rpc.services.desk.catchup run-once|watchdog",
              file=sys.stderr)
        return 2
    if scheduler is None:
        setup()
        try:
            scheduler = build_scheduler(env)
        except ConfigError as exc:
            print(f"catch-up: configuration: {exc}", file=sys.stderr)
            return 2
    if argv == ["watchdog"]:
        if not scheduler._catchup.enabled:
            print("catch-up: disabled (QT_CATCHUP_ENABLED=0)")
            return 0
        now_local = scheduler.local_now()
        if not scheduler.in_window(now_local):
            print("catch-up: outside the window")
            return 0
        age = scheduler.heartbeat_age_s()
        limit = scheduler.watchdog_limit_s(now_local)
        if age is not None and age <= limit:
            print(f"catch-up: ok (last pass {age / 60:.0f} min ago)")
            return 0
        scheduler.files.line("watchdog: no pass for "
                             + ("ever" if age is None else f"{age / 60:.0f} min")
                             + "; running one")
        print("catch-up: the in-process scheduler is late; running a pass")
    outcomes = scheduler.run_pass()
    if outcomes is None:
        print("catch-up: another pass is running")
        return 0
    for pdir, step in outcomes:
        print(f"catch-up: {pdir}: {step.action} {step.date or ''} {step.reason}".rstrip())
    return 1 if any(step.action == ALERT for _, step in outcomes) else 0


if __name__ == "__main__":
    sys.exit(main())
