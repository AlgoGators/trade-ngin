"""The QT catch-up scheduler (contract C6, ruling 29): the model runs, in the rpc host.

It replaces the host cron's single 10:15 model run. A background task of the desk service
(`desk.catchup`, service.py) ticks every minute; a pass is due once per QT_CATCHUP_EVERY_MIN
(30) slot inside QT_CATCHUP_WINDOW (06:00-22:00 America/New_York), every day. A pass holds
<QT_LOCK_DIR>/qt-catchup.lock (so the host watchdog's pass and the in-process one never overlap)
and walks each portfolio of QT_CATCHUP_PORTFOLIOS:

    last = the newest date with a book (qt on a desk-editable portfolio, else system; a
           desk-editable portfolio with no qt book yet starts after its newest system day)
    while last < today:
        desk-editable and `last` unpublished:
            a trading day      -> wait for the desk's publish (no alert; a reminder after 24 h)
            a non-trading day  -> alert (the model run should have auto-published it)
        day = last + 1
        today, a trading day, before QT_CATCHUP_TODAY_NOT_BEFORE (10:15) -> wait (T-1 data)
        run the model for `day`; exit 0 with a book written -> last = day, continue
        anything else -> alert, stop this portfolio until the next pass

A "trading day" is a book date whose T-1 is an open session on the runner's calendar
(holidays.json, the file HolidayChecker reads; weekends closed), the same test the engine uses
to auto-publish a non-trading day (C5). A calendar that does not cover the dates is an alert,
never a guess.

Each model run is exactly what scripts/qt_model_run.sh runs: `<QT_ENGINE_BINARY>
--portfolio-config <dir> --date <day>` in QT_ENGINE_CWD, under <QT_LOCK_DIR>/<portfolio_id>.lock,
the lock desk jobs take. The scheduler holds that flock itself (flock(2), the same lock flock(1)
takes) around a re-check of the database and the run, so a day another run has just written is
never run twice. QT_JOB_TIMEOUT_S bounds the run.

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
from typing import Callable, Dict, Iterator, List, Optional, Protocol, Sequence, Set, Tuple

from .config import (CONNECT_TIMEOUT_S, CatchupSettings, CommandSettings, ConfigError, DbConfig,
                     load_settings)
from .jobs import Runner, SubprocessRunner, tail
from .override_mail import ApproverError, Sender, load_smtp, parse_approvers, send_smtp
from .portfolios import portfolios_root

log = logging.getLogger("desk.catchup")

UTC = dt.timezone.utc
CATCHUP_LOG = "qt-catchup.log"
ENGINE_LOG = "qt-engine-runs.log"
ALERT_STATE = "qt-alerts.json"
HEARTBEAT = "qt-catchup.heartbeat"
SCHEDULER_LOCK = "qt-catchup.lock"
# A pass never walks further than this many days in one go (a sanity bound, not a policy).
MAX_DAYS_PER_PASS = 62


def _zone(name: str):
    from zoneinfo import ZoneInfo

    return ZoneInfo(name)


# -- the calendar -------------------------------------------------------------------------------

class CalendarError(RuntimeError):
    pass


def holidays_path(engine_cwd: str, env=None) -> str:
    """HolidayChecker::resolve_holidays_path, from the engine's working directory."""
    env = os.environ if env is None else env
    if env.get("TRADE_NGIN_HOLIDAYS_JSON"):
        return env["TRADE_NGIN_HOLIDAYS_JSON"]
    candidates = [os.path.join(engine_cwd, "include/trade_ngin/core/holidays.json"),
                  os.path.join(engine_cwd, "holidays.json"), "/etc/trade_ngin/holidays.json"]
    for path in candidates:
        if os.path.exists(path):
            return path
    return candidates[0]


class Calendar:
    """The runner's market calendar: weekends and the holidays.json closures."""

    def __init__(self, holidays: Set[dt.date], years: Set[int]):
        self._holidays = holidays
        self._years = years

    @classmethod
    def load(cls, path: str) -> "Calendar":
        try:
            data = json.loads(Path(path).read_text(encoding="utf-8"))
            holidays, years = set(), set()
            for year, items in data.items():
                years.add(int(year))
                for item in items:
                    holidays.add(dt.date.fromisoformat(item["date"]))
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
            raise CalendarError(f"cannot load the market calendar {path}: "
                                f"{type(exc).__name__}") from None
        if not years:
            raise CalendarError(f"the market calendar {path} is empty")
        return cls(holidays, years)

    def covers(self, day: dt.date) -> bool:
        return day.year in self._years

    def is_session(self, day: dt.date) -> bool:
        if not self.covers(day):
            raise CalendarError(f"the market calendar does not cover {day}")
        return day.weekday() < 5 and day not in self._holidays

    def is_trading_day(self, book_date: dt.date) -> bool:
        """A book date is a trading day when its T-1 was an open session (C5)."""
        return self.is_session(book_date - dt.timedelta(days=1))


# -- the database -------------------------------------------------------------------------------

class DayStore(Protocol):
    def last_book_date(self, portfolio_id: str, book: str) -> Optional[dt.date]: ...

    def published_at(self, portfolio_id: str, date: dt.date) -> Optional[dt.datetime]: ...

    def book_written_at(self, portfolio_id: str, date: dt.date,
                        book: str) -> Optional[dt.datetime]: ...


class PostgresDayStore:
    def __init__(self, db: DbConfig, connect_timeout: int = CONNECT_TIMEOUT_S):
        self._db = db
        self._connect_timeout = connect_timeout

    def _one(self, sql: str, args):
        import psycopg

        from .store import StoreError
        try:
            with psycopg.connect(**self._db.conninfo_kwargs(self._connect_timeout),
                                 autocommit=True) as conn, conn.cursor() as cur:
                cur.execute(sql, args)
                rec = cur.fetchone()
                return rec[0] if rec else None
        except Exception as exc:
            raise StoreError(f"database error ({type(exc).__name__})") from exc

    def last_book_date(self, portfolio_id, book):
        return self._one("SELECT max(date) FROM trading.live_results "
                         "WHERE portfolio_id = %s AND portfolio_type = %s", (portfolio_id, book))

    def published_at(self, portfolio_id, date):
        return self._one("SELECT max(published_at) FROM trading.live_run_metadata "
                         "WHERE portfolio_id = %s AND date = %s", (portfolio_id, date))

    def book_written_at(self, portfolio_id, date, book):
        # created_at is timestamp without time zone in the writer's session zone (R12).
        return self._one("SELECT max(created_at)::timestamptz FROM trading.live_results "
                         "WHERE portfolio_id = %s AND date = %s AND portfolio_type = %s",
                         (portfolio_id, date, book))


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
              message: str) -> bool:
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
        mailed, why = self._mail(portfolio_dir, portfolio_id, date, reason, message)
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

    def _mail(self, portfolio_dir, portfolio_id, date, reason, message) -> Tuple[bool, str]:
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

        subject = f"QT alert: {portfolio_id} {date or ''} {reason}".strip()
        body = ("<html><body>"
                f"<p><b>{html.escape(portfolio_id)}</b>, book date "
                f"<b>{html.escape(str(date or '-'))}</b>: {html.escape(reason)}</p>"
                f"<pre>{html.escape(message)}</pre>"
                "<p>From the QT catch-up scheduler on engine-rpc. Details: "
                "/home/ubuntu/qt-engine/logs/qt-catchup.log and qt-engine-runs.log. This alert "
                "is sent at most once a day per portfolio, date and reason.</p>"
                "</body></html>")
        try:
            self._sender(smtp, [president], subject, body)
        except Exception as exc:
            return False, f"send failed: {type(exc).__name__}"
        return True, ""


# -- the scheduler ------------------------------------------------------------------------------

RUN, WAIT_PUBLISH, WAIT_TIME, UP_TO_DATE, ALERT = (
    "run", "wait_publish", "wait_time", "up_to_date", "alert")


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


class CatchupScheduler:
    def __init__(self, store: DayStore, settings: CommandSettings, catchup: CatchupSettings,
                 runner: Optional[Runner] = None, sender: Sender = send_smtp,
                 clock: Optional[Callable[[], dt.datetime]] = None,
                 calendar_loader: Optional[Callable[[], Calendar]] = None,
                 lock: Optional[LockFactory] = None):
        self._store = store
        self._settings = settings
        self._catchup = catchup
        self._runner = runner or SubprocessRunner()
        self._clock = clock or (lambda: dt.datetime.now(UTC))
        self._tz = _zone(catchup.timezone)
        self._calendar_loader = calendar_loader or (
            lambda: Calendar.load(holidays_path(settings.engine_cwd)))
        self._lock = lock or file_lock
        self.files = FileLog(settings.log_dir, self._clock)
        self.alerter = Alerter(settings, self.files, sender, self._clock, self._tz)
        self._last_slot: Optional[Tuple[dt.date, int, int]] = None

    # -- when -----------------------------------------------------------------------------------

    def local_now(self) -> dt.datetime:
        return self._clock().astimezone(self._tz)

    def in_window(self, now_local: dt.datetime) -> bool:
        t = now_local.time()
        return self._catchup.window_start <= t <= self._catchup.window_end

    def _slot(self, now_local: dt.datetime) -> Tuple[dt.date, int, int]:
        every = self._catchup.every_minutes
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
        """Walk one portfolio forward. Returns the step it stopped on."""
        try:
            info = read_portfolio(self._settings.config_dir, portfolio_dir)
        except ConfigError as exc:
            self.alerter.alert(portfolio_dir, portfolio_dir, None, "config", str(exc))
            return Step(ALERT, reason="config", message=str(exc))
        try:
            calendar = self._calendar_loader()
        except CalendarError as exc:
            self.alerter.alert(portfolio_dir, info.portfolio_id, None, "calendar", str(exc))
            return Step(ALERT, reason="calendar", message=str(exc))
        lock_path = os.path.join(self._settings.lock_dir, f"{info.portfolio_id}.lock")
        step = Step(UP_TO_DATE)
        for _ in range(MAX_DAYS_PER_PASS):
            with self._lock(lock_path, self._catchup.lock_wait_s) as held:
                if not held:
                    self.files.line(f"{info.portfolio_id}: busy (its lock is held); next pass")
                    return Step(WAIT_TIME, reason="busy")
                step = self.next_step(info, calendar)
                if step.action != RUN:
                    break
                result_step = self._run_model(info, step.date)
            if result_step is not None:
                step = result_step
                break
        self._report(info, step, calendar)
        return step

    def next_step(self, info: PortfolioInfo, calendar: Calendar) -> Step:
        now_local = self.local_now()
        today = now_local.date()
        pid = info.portfolio_id
        book = "qt" if info.desk_editable else "system"
        last = self._store.last_book_date(pid, book)
        from_system = False
        if last is None and info.desk_editable:
            last = self._store.last_book_date(pid, "system")  # the first desk day (ruling 27)
            from_system = last is not None
        try:
            if last is not None and last >= today:
                return Step(UP_TO_DATE, last)
            if last is not None and info.desk_editable and not from_system:
                if self._store.published_at(pid, last) is None:
                    if calendar.is_trading_day(last):
                        return Step(WAIT_PUBLISH, last, "unpublished",
                                    f"{last} is waiting for the desk's publish")
                    return Step(ALERT, last, "not_auto_published",
                                f"{last} is a non-trading day but its model run did not "
                                "publish it (system:non-trading-day); the next day cannot run")
            day = today if last is None else last + dt.timedelta(days=1)
            trading = calendar.is_trading_day(day)
        except CalendarError as exc:
            return Step(ALERT, last, "calendar", str(exc))
        if day == today and trading and now_local.time() < self._catchup.today_not_before:
            return Step(WAIT_TIME, day, "before_data",
                        f"today's run waits until {self._catchup.today_not_before:%H:%M}")
        return Step(RUN, day)

    def _run_model(self, info: PortfolioInfo, day: dt.date) -> Optional[Step]:
        """Run the model for `day` (the portfolio lock is held). None when the book was written,
        else the alert step."""
        s = self._settings
        argv = [s.engine_binary, "--portfolio-config", info.dir, "--date", day.isoformat()]
        self.files.line(f"{info.portfolio_id}: model run {day} ({' '.join(argv)})")
        started = time.monotonic()
        with self.files.append(ENGINE_LOG) as f:
            if f is not None:
                f.write(f"=== {self._clock().isoformat(timespec='seconds')} {info.portfolio_id} "
                        f"({info.dir}) model run {day}\n")
            res = self._runner.run(argv, s.engine_cwd, s.job_timeout_s, copy_to=f)
            if f is not None:
                f.write(f"=== exit {res.rc}{' (timed out)' if res.timed_out else ''}\n")
        err = tail(res.stderr, 5) or tail(res.stdout, 5)
        elapsed = round(time.monotonic() - started, 1)
        log.info("catch-up model run", extra={"portfolio_id": info.portfolio_id,
                                              "date": str(day), "rc": res.rc,
                                              "timed_out": res.timed_out, "error": res.error,
                                              "elapsed_ms": elapsed * 1000,
                                              "stderr_tail": err})
        self.files.line(f"{info.portfolio_id}: model run {day} exit {res.rc} in {elapsed}s")
        detail = "\n".join(err)
        if res.error is not None:
            return Step(ALERT, day, "engine_not_started", res.error)
        if res.timed_out:
            return Step(ALERT, day, "timeout",
                        f"the model run was killed after {s.job_timeout_s:g}s\n{detail}")
        if res.rc != 0:
            reason = "refused" if res.rc == 2 or "QT_REFUSED" in detail or \
                "QT_ALERT" in detail else "failed"
            return Step(ALERT, day, reason, f"the model run exited {res.rc}\n{detail}")
        book = "qt" if info.desk_editable else "system"
        latest = self._store.last_book_date(info.portfolio_id, book)
        if latest is None or latest < day:
            return Step(ALERT, day, "no_book",
                        f"the model run exited 0 but wrote no {book} book for {day}\n{detail}")
        return None

    def _report(self, info: PortfolioInfo, step: Step, calendar: Calendar) -> None:
        pid = info.portfolio_id
        if step.action == ALERT:
            self.alerter.alert(info.dir, pid, step.date, step.reason, step.message)
        elif step.action == WAIT_PUBLISH:
            self.files.line(f"{pid}: {step.message}")
            written = self._store.book_written_at(pid, step.date, "qt")
            if written is not None:
                waited_h = (self._clock() - written).total_seconds() / 3600
                if waited_h > self._catchup.reminder_after_h:
                    self.alerter.alert(info.dir, pid, step.date, "unpublished_reminder",
                                       f"{step.date} has waited {waited_h:.0f} h for the desk's "
                                       "publish (its model run finished at "
                                       f"{written.isoformat(timespec='minutes')}); the days "
                                       "after it cannot run until it is published")
        elif step.action == WAIT_TIME:
            self.files.line(f"{pid}: {step.message or step.reason}")
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
        if not scheduler.in_window(scheduler.local_now()):
            print("catch-up: outside the window")
            return 0
        age = scheduler.heartbeat_age_s()
        limit = 2 * scheduler._catchup.every_minutes * 60 + 900
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
