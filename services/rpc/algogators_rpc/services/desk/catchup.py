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

After each pass (D-40): a day of the last MISSED_SEND_ALERT_DAYS that was published but never
e-mailed before it passed (an approval whose 09:30 send did not happen) is not sent late; the
President is alerted, once a day. A caught-up day is never e-mailed and was alerted already.

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

THE DATA-DRIVEN RUN (QT_EVENING_RUN=1, the default; the variable keeps its first name). Interim
design of https://github.com/AlgoGators/trade-ngin/issues/191, to be event-driven over NATS by
https://github.com/AlgoGators/trade-ngin/issues/192. data-ngin pulls at 04:30 ET once the
vendor's ~8 h delayed historical data has bar D (the UTC day that ended before the pull: on day T,
bar T-1) and writes ops.data_ready (migration 028) around 04:35-05:00: one row per
(dataset, bar_date) with its status. Each pass reads the newest row of each dataset of
QT_DATA_READY_DATASET (futures_ohlcv_1d; a comma-separated list), plus the per-contract pull's
futures_contract_ohlcv_1d for a portfolio whose portfolio.json turns per_contract_rolls on (rolls
step 4: a late per-contract row delays the run like a late continuous row, with the same alert;
combine_ready), for bar date D and builds the books of T = the next trading day after D
(trading_days.py, the engine's holiday calendar: Friday's, Saturday's and Sunday's bars all build
Monday, a holiday is skipped). Normally T is today, and today's model run is the primary run:

    today's model run (not before QT_CATCHUP_TODAY_NOT_BEFORE) waits for a usable row for the
    previous trading day: before QT_DATA_READY_ALERT_AT (06:00) it waits quietly (the pull may
    still be running), from then on it alerts; a failed or stale row alerts at once
    the data is usable when its status is complete, or partial with every missing symbol's last
    bar within the engine's live.data_staleness_tolerance_days of T - 1 (the engine's own
    FEED_HOLE guard decides again inside the run; a refusal is an alert, nothing is built);
    failed, or a stale partial, is an alert and nothing is built
    when T is after today (data that reaches past today, e.g. Saturday's run after its 10:00
    fallback with Friday's or Sunday's bars in), the engine's day chain is every calendar day, so
    the days between today and T are built in order. On the desk portfolio (D-63, decision 6)
    every one of them waits like a weekday: the next day is built once today is published, and
    the chain stops at the first day after today that is not published; that day, once it is
    today, waits for the desk's approval or its own 10:00 fallback, with the alert, and the
    chain goes on (ruling 29). Each day built is reviewed at once (the review e-mail below),
    and the desk may approve it at once (Dominick 2026-10-18: no reason to wait once the data
    is in), which builds the next: Friday's bars give Saturday, Sunday and Monday as fast as the
    desk approves them. Only the desk publishes a day ahead of its own day; the 10:00 fallback
    acts on today (system:fallback-nontrading is no longer written). On Sunday from 18:00 the
    next trading day's book not approved yet reminds the desk, the VP and the President, once.
    The twin's days are built ahead and published model-only when they become today
    T is (re)built once per data_ready.finished_at: a re-pull rebuilds it, unless T is published
    (frozen: never rebuilt). A desk portfolio whose day has the desk's saved edits keeps them: the
    engine's model run then rewrites only the system book and keeps qt_proposal and qt (QT_SEED
    kept, qt::desk_edit_done); otherwise all three are rewritten
    after a build of a desk portfolio's day (T, or a day on the way to it), while it is
    unpublished and before its 10:00, QT_REVIEW_RECIPIENTS get the review e-mail
    (review_mail.py), once per build
    from QT_DATA_READY_ALERT_AT (06:00), every day: no non-failed row for the previous trading
    day is one alert to the President
    09:30 send, 10:00 fallback and the approval rules are unchanged.

After every pass, each desk action that finished since the last one (a command row) and each new
settings version is e-mailed to the VP and the President (activity_mail.py), one e-mail each.

Recommended host settings with it: QT_CATCHUP_TODAY_NOT_BEFORE=04:30, QT_CATCHUP_WINDOW=04:00-22:00,
QT_CATCHUP_BUSY_WINDOW=04:30-10:30 (the code defaults stay 06:45 / 06:30-22:00 / 06:30-10:30, the
QT_EVENING_RUN=0 schedule). QT_EVENING_RUN=0 restores the 06:45-only schedule above, unchanged.

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
from .config import (CONNECT_TIMEOUT_S, CONTRACT_DATA_READY_DATASET, CatchupSettings,
                     CommandSettings, ConfigError, DbConfig, load_settings)
from .jobs import Runner, RunResult, SubprocessRunner, tail
from .override_mail import ApproverError, Sender, load_smtp, parse_approvers, send_smtp
from .portfolios import portfolios_root
from . import activity_mail, review_mail
from .trading_days import (TradingCalendar, load_calendar, per_contract_rolls,
                           resolve_holidays_path, staleness_tolerance_days)

log = logging.getLogger("desk.catchup")

UTC = dt.timezone.utc
CATCHUP_LOG = "qt-catchup.log"
ENGINE_LOG = "qt-engine-runs.log"
ALERT_STATE = "qt-alerts.json"
HEARTBEAT = "qt-catchup.heartbeat"
SCHEDULER_LOCK = "qt-catchup.lock"
# A pass never takes more than this many steps per portfolio (a sanity bound, not a policy).
MAX_DAYS_PER_PASS = 62
# D-40: how many past days a published day that was never e-mailed is alerted on.
MISSED_SEND_ALERT_DAYS = 3
# How far back the last bar of a symbol data-ngin reported missing is looked for.
MISSING_LOOKBACK_DAYS = 60
# Dominick 2026-10-18: on Sunday from 18:00 New York, the next trading day's book not approved
# yet reminds the QT desk, the VP and the President, once.
SUNDAY_REMINDER_AT = dt.time(18, 0)
# The most desk actions (and settings versions) the log e-mails in one pass; the rest go next.
ACTIVITY_BATCH = 50
# The log reads only rows finished (or settings saved) at least this long ago: a transaction that
# set finished_at = now() but commits later would otherwise land behind the log's mark.
ACTIVITY_SETTLE = dt.timedelta(minutes=2)


def _zone(name: str):
    from zoneinfo import ZoneInfo

    return ZoneInfo(name)


# -- the database -------------------------------------------------------------------------------

@dataclass(frozen=True)
class DataReady:
    """A row of ops.data_ready (migration 028), written by data-ngin."""
    dataset: str
    bar_date: dt.date
    expected: int
    loaded: int
    missing: Tuple[str, ...]
    status: str                 # complete | partial | failed
    run_id: Optional[str]
    finished_at: dt.datetime
    # Rolls step 4: the rows this one combines (combine_ready), each dataset's own; () on a row
    # read from the table.
    parts: Tuple["DataReady", ...] = ()


STATUS_ORDER = ("complete", "partial")


def combine_ready(rows: Sequence[DataReady]) -> DataReady:
    """The data every dataset a run waits for has (rolls step 4, ROLLS_STEP1_PLAN section 5a): one
    row standing for the newest row of each dataset. Its bar date is the OLDEST of theirs, so a
    late dataset holds the run back as a late continuous row does; the rows on that date decide
    the status (the worst: failed, any other, partial, complete), the counts, the missing symbols
    and finished_at (the latest of theirs: the data was in once the last of them finished). A
    dataset already on a later date is in for this one. One row is returned as it is."""
    if len(rows) == 1:
        return rows[0]
    bar_date = min(r.bar_date for r in rows)
    on = [r for r in rows if r.bar_date == bar_date]
    worst = max(on, key=lambda r: (STATUS_ORDER.index(r.status) if r.status in STATUS_ORDER
                                   else len(STATUS_ORDER) + (r.status == "failed")))
    missing: List[str] = []
    for r in on:
        missing.extend(m for m in r.missing if m not in missing)
    return DataReady("+".join(r.dataset for r in rows), bar_date,
                     sum(r.expected for r in on), sum(r.loaded for r in on), tuple(missing),
                     worst.status, ",".join(r.run_id or "-" for r in on),
                     max(r.finished_at for r in on), tuple(on))


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

    # -- the data-driven run (issues #191, #192) --

    def latest_data_ready(self, dataset: str) -> Optional[DataReady]:
        """The ops.data_ready row of the dataset with the newest bar_date."""

    def last_bar_dates(self, symbols: Sequence[str], since: dt.date) -> Dict[str, dt.date]:
        """The newest futures_data.ohlcv_1d bar date of each symbol, from `since` on."""

    def last_contract_bar_dates(self, roots: Sequence[str],
                                since: dt.date) -> Dict[str, dt.date]:
        """The newest futures_data.contract_ohlcv_1d bar date of each root (the per-contract
        row's missing names roots), from `since` on."""

    def desk_edited(self, portfolio_id: str, date: dt.date) -> bool:
        """The desk saved the day (a done save, or an approved override): what the engine's
        qt::desk_edit_done reads to keep qt_proposal and qt on a model re-run."""

    def day_books(self, portfolio_id: str, date: dt.date) -> review_mail.DayBooks:
        """{book: {symbol: Line}} of the system and qt books of the day."""

    def last_published_before(self, portfolio_id: str, date: dt.date) -> Optional[dt.date]: ...

    def email_held(self, portfolio_id: str, date: dt.date) -> bool:
        """D-66: the day's approval asked for no e-mail (the newest done publish row that
        approved it has payload.email false); the desk sends it with Send when it wants."""

    def unsent_days(self, portfolio_id: str, since: dt.date,
                    before: dt.date) -> List[Tuple[dt.date, str]]:
        """(date, published_by) of each day in [since, before) that is published and was never
        e-mailed (sent_at NULL on all its rows), oldest first."""

    # -- the VP and President's log of desk actions (activity_mail.py) --

    def activity_mark(self) -> activity_mail.Mark:
        """Where the log starts on a first start: the newest final command (finished_at, id)
        and the newest settings version id now."""

    def desk_commands_after(self, mark: activity_mail.Mark, before: dt.datetime,
                            limit: int) -> List[Any]:
        """Command rows (command_store.CommandRow) that reached a final status after the mark
        and before `before`, in (finished_at, id) order."""

    def settings_versions_after(self, after_id: int, before: dt.datetime,
                                limit: int) -> List[activity_mail.SettingsVersion]:
        """trading.strategy_config versions with an id above `after_id`, saved before `before`,
        in id order, each with the overrides of the version before it."""


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
        # created_at is timestamp without time zone; the engine writes it in UTC (R12), so
        # read it as UTC whatever this session's zone is.
        return self._one("SELECT max(created_at) AT TIME ZONE 'UTC' FROM trading.live_results "
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

    def latest_data_ready(self, dataset):
        def q(cur):
            cur.execute("SELECT dataset, bar_date, expected, loaded, missing, status, run_id, "
                        "finished_at FROM ops.data_ready WHERE dataset = %s "
                        "ORDER BY bar_date DESC LIMIT 1", (dataset,))
            r = cur.fetchone()
            if r is None:
                return None
            return DataReady(r[0], r[1], int(r[2]), int(r[3]), tuple(r[4] or ()), r[5], r[6],
                             r[7])
        return self._run(q)

    def last_bar_dates(self, symbols, since):
        if not symbols:
            return {}

        def q(cur):
            # Bars are keyed at 00:00 UTC of their date; the session is UTC.
            cur.execute("SELECT symbol, max(time)::date FROM futures_data.ohlcv_1d "
                        "WHERE symbol = ANY(%s) AND time >= %s GROUP BY symbol",
                        (list(symbols), since))
            return {sym: d for sym, d in cur.fetchall() if d is not None}
        return self._run(q)

    def last_contract_bar_dates(self, roots, since):
        if not roots:
            return {}

        def q(cur):
            cur.execute("SELECT root, max(date) FROM futures_data.contract_ohlcv_1d "
                        "WHERE root = ANY(%s) AND date >= %s GROUP BY root",
                        (list(roots), since))
            return {root: d for root, d in cur.fetchall() if d is not None}
        return self._run(q)

    def desk_edited(self, portfolio_id, date):
        return bool(self._one(
            "SELECT count(*) FROM trading.position_overrides WHERE portfolio_id = %s "
            "AND date = %s AND status = 'done' AND (kind = 'save' OR (kind = "
            "'override_decision' AND COALESCE((payload->>'approved')::boolean, false)))",
            (portfolio_id, date)))

    def day_books(self, portfolio_id, date):
        def q(cur):
            cur.execute("SELECT portfolio_type, symbol, sum(quantity), max(moved_by) "
                        "FROM trading.positions WHERE portfolio_id = %s AND date = %s "
                        "AND portfolio_type IN ('system', 'qt') "
                        "GROUP BY portfolio_type, symbol", (portfolio_id, date))
            out: review_mail.DayBooks = {"system": {}, "qt": {}}
            for book, symbol, qty, moved_by in cur.fetchall():
                out.setdefault(book, {})[symbol] = review_mail.Line(float(qty or 0), moved_by)
            return out
        return self._run(q)

    def last_published_before(self, portfolio_id, date):
        return self._one("SELECT max(date) FROM trading.live_run_metadata WHERE portfolio_id = %s "
                         "AND date < %s AND published_at IS NOT NULL", (portfolio_id, date))

    def email_held(self, portfolio_id, date):
        def q(cur):
            cur.execute("SELECT payload->>'email' FROM trading.position_overrides "
                        "WHERE portfolio_id = %s AND date = %s AND kind = 'publish' "
                        "AND status = 'done' AND result ? 'published_at' "
                        "ORDER BY id DESC LIMIT 1", (portfolio_id, date))
            rec = cur.fetchone()
            return rec is not None and rec[0] == "false"
        return self._run(q)

    def unsent_days(self, portfolio_id, since, before):
        def q(cur):
            cur.execute("SELECT date, COALESCE(max(published_by), '') "
                        "FROM trading.live_run_metadata WHERE portfolio_id = %s "
                        "AND date >= %s AND date < %s GROUP BY date "
                        "HAVING max(published_at) IS NOT NULL AND max(sent_at) IS NULL "
                        "ORDER BY date", (portfolio_id, since, before))
            return [(r[0], r[1]) for r in cur.fetchall()]
        return self._run(q)

    _FINAL_SQL = "status IN ('done', 'refused', 'failed') AND finished_at IS NOT NULL"

    def activity_mark(self):
        def q(cur):
            cur.execute("SELECT finished_at, id FROM trading.position_overrides WHERE "
                        f"{self._FINAL_SQL} ORDER BY finished_at DESC, id DESC LIMIT 1")
            rec = cur.fetchone()
            cur.execute("SELECT COALESCE(max(id), 0) FROM trading.strategy_config")
            settings_id = cur.fetchone()[0]
            return activity_mail.Mark(rec[0] if rec else None, rec[1] if rec else 0,
                                      int(settings_id))
        return self._run(q)

    def desk_commands_after(self, mark, before, limit):
        from .command_store import _SELECT, CommandRow

        def q(cur):
            if mark.command_at is None:
                cur.execute(f"SELECT {_SELECT} FROM trading.position_overrides WHERE "
                            f"{self._FINAL_SQL} AND finished_at < %s "
                            "ORDER BY finished_at, id LIMIT %s", (before, limit))
            else:
                cur.execute(f"SELECT {_SELECT} FROM trading.position_overrides WHERE "
                            f"{self._FINAL_SQL} AND finished_at < %s "
                            "AND (finished_at, id) > (%s, %s) ORDER BY finished_at, id LIMIT %s",
                            (before, mark.command_at, mark.command_id, limit))
            return [CommandRow.from_record(r) for r in cur.fetchall()]
        return self._run(q)

    def settings_versions_after(self, after_id, before, limit):
        def q(cur):
            cur.execute("SELECT c.id, c.portfolio_id, c.version, c.reason, c.created_by, "
                        "c.created_at, c.overrides, p.overrides "
                        "FROM trading.strategy_config c LEFT JOIN trading.strategy_config p "
                        "ON p.portfolio_id = c.portfolio_id AND p.version = c.version - 1 "
                        "WHERE c.id > %s AND c.created_at < %s ORDER BY c.id LIMIT %s",
                        (after_id, before, limit))
            return [activity_mail.SettingsVersion(r[0], r[1], r[2], r[3], r[4], r[5],
                                                  r[6] or {}, r[7] or {})
                    for r in cur.fetchall()]
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
              message: str, subject: Optional[str] = None,
              also_to: Sequence[str] = ()) -> bool:
        """Returns True when this call raised the alert (False: already raised today).
        `also_to`: addresses e-mailed with the president (a reminder to the desk)."""
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
        mailed, why = self._mail(portfolio_dir, portfolio_id, date, reason, message, subject,
                                 also_to)
        if mailed:
            self._files.line(f"ALERT {portfolio_id} {date or '-'} {reason}: e-mailed to the "
                             "president" + (f" and {', '.join(also_to)}" if also_to else ""))
        else:
            self._files.line(f"ALERT {portfolio_id} {date or '-'} {reason}: not e-mailed ({why})")
            if why.startswith("send failed"):
                return True  # not recorded: the next pass tries the e-mail again
        state[key] = today.isoformat()
        self._save(state, today)
        return True

    def _mail(self, portfolio_dir, portfolio_id, date, reason, message,
              subject, also_to=()) -> Tuple[bool, str]:
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
        to: List[str] = []
        for address in [president, *also_to]:
            address = address.strip()
            if address and address.lower() not in {a.lower() for a in to}:
                to.append(address)
        try:
            self._sender(smtp, to, subject, body)
        except Exception as exc:
            return False, f"send failed: {type(exc).__name__}"
        return True, ""


# -- the scheduler ------------------------------------------------------------------------------

(RUN, FALLBACK, SEND, PUBLISH_MODEL, WAIT_PUBLISH, WAIT_TIME, UP_TO_DATE, ALERT) = (
    "run", "fallback", "send", "publish_model", "wait_publish", "wait_time", "up_to_date",
    "alert")
REVIEW = "review_mail"
ACTIONS = (RUN, FALLBACK, SEND, PUBLISH_MODEL, REVIEW)
# A FALLBACK step's reason: the 10:00 fallback of today (e-mailed), the catch-up of a missed past
# day (not e-mailed, alerted) and the data-driven run's non-trading day ahead of the target
# (neither).
FALLBACK_TODAY, FALLBACK_PAST, FALLBACK_NONTRADING = "fallback", "caught_up", "nontrading"


@dataclass(frozen=True)
class Step:
    action: str
    date: Optional[dt.date] = None
    reason: str = ""
    message: str = ""
    # A data-driven build: the ops.data_ready row it builds from (its book must be newer).
    ready: Optional[DataReady] = None


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
                 lock: Optional[LockFactory] = None, dispatcher=None,
                 calendar: Optional[TradingCalendar] = None):
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
        # The data-driven run (issues #191, #192): the engine's trading calendar (loaded on first
        # use), the review e-mail's sender and its record.
        self._calendar = calendar
        self._sender = sender
        self.reviews = review_mail.ReviewState(settings.log_dir)
        self.activities = activity_mail.ActivityState(settings.log_dir)

    @property
    def calendar(self) -> TradingCalendar:
        if self._calendar is None:
            self._calendar = load_calendar(self._catchup.holidays_path or resolve_holidays_path(
                self._settings.engine_cwd))
        return self._calendar

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
            if self._catchup.evening_run and self._catchup.portfolios:
                try:
                    self._data_ready_check()
                except Exception as exc:
                    log.error("the data-ready check failed", exc_info=True,
                              extra={"error": type(exc).__name__})
                    self.alerter.alert(self._catchup.portfolios[0],
                                       self._catchup.data_ready_datasets[0], None,
                                       "scheduler_error",
                                       f"the data-ready check failed: {type(exc).__name__}: {exc}")
            try:
                self._activity_mail()
            except Exception as exc:
                log.error("the desk-action log failed", exc_info=True,
                          extra={"error": type(exc).__name__})
                if self._catchup.portfolios:
                    self.alerter.alert(self._catchup.portfolios[0], "qt", None,
                                       "activity_mail_failed",
                                       f"the VP and President's log of desk actions failed: "
                                       f"{type(exc).__name__}: {exc}")
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
        self._missed_send_check(info)
        self._sunday_reminder(info)
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
        if not self._catchup.evening_run:
            if last is not None and last > today:
                return Step(UP_TO_DATE, last)
            return self._day_step(info, now_local, last, from_system)
        # The data-driven run (https://github.com/AlgoGators/trade-ngin/issues/191 and /192).
        # Books ahead of today were built by it; the chain is contiguous, so today has its book and
        # today's own duties (the 10:00 fallback, the model twin's publish, the 09:30 send) come
        # first. Then the build of the next trading day's books, and the desk's review e-mail.
        head = today if last is not None and last > today and not from_system else last
        step = self._day_step(info, now_local, head, from_system)
        if step.action not in (WAIT_PUBLISH, UP_TO_DATE) or last is None or last < today:
            return step
        return self._evening_step(info, now_local, last) or step

    def _day_step(self, info: PortfolioInfo, now_local: dt.datetime, last: Optional[dt.date],
                  from_system: bool) -> Step:
        """The day's flow (contract C7) up to today, `last` being the newest book (at most
        today)."""
        today = now_local.date()
        pid = info.portfolio_id
        if info.desk_editable and last is not None and not from_system \
                and self._store.published_at(pid, last) is None:
            if last < today:
                return self._blocked(pid, last, FALLBACK) or Step(
                    FALLBACK, last, FALLBACK_PAST, f"{last} was never approved")
            if now_local >= cutoff.cutoff_time(today):
                return self._blocked(pid, last, FALLBACK) or Step(
                    FALLBACK, last, FALLBACK_TODAY, f"{last} was not approved by 10:00")
            return Step(WAIT_PUBLISH, last, "unpublished",
                        f"{last} waits for the desk's approval (by 09:30; the 10:00 fallback "
                        "sends the model's book)")
        if last is None or last < today or from_system:
            # (from_system and last == today: today's model run wrote system but no qt; again)
            day = today if last is None or last >= today else last + dt.timedelta(days=1)
            if day == today and now_local.time() < self._catchup.today_not_before:
                return Step(WAIT_TIME, day, "before_data",
                            f"today's run waits until {self._catchup.today_not_before:%H:%M}")
            if day == today and self._catchup.evening_run:
                # Today has no book yet (else `last` would be today): its model run waits for
                # the previous trading day's data (ops.data_ready).
                gate = self._morning_gate(info, today)
                if gate is not None:
                    return gate
            return self._blocked(pid, day, RUN) or Step(RUN, day)
        # today has its book
        if not info.desk_editable and self._store.published_at(pid, today) is None:
            return Step(PUBLISH_MODEL, today)
        published = self._store.published_at(pid, today)
        if published is not None and now_local >= cutoff.send_time(today) \
                and self._store.sent_at(pid, today) is None:
            if self._store.email_held(pid, today):
                return Step(UP_TO_DATE, today, "not_sent",
                            "approved without the e-mail (D-66): the desk sends it with Send")
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

    # -- the data-driven run (https://github.com/AlgoGators/trade-ngin/issues/191 and /192) ----
    # Interim: Postgres polling of ops.data_ready once a pass. #191 makes strategy runs
    # event-driven on declared inputs; #192 replaces this polling with NATS JetStream.

    def _datasets(self, portfolio_dir: str) -> Tuple[str, ...]:
        """The ops.data_ready datasets the portfolio's run waits for: QT_DATA_READY_DATASET's, and
        the per-contract pull's when its portfolio.json turns per_contract_rolls on (rolls step 4,
        ROLLS_STEP1_PLAN section 5a: a late per-contract row delays the run like a late
        continuous row)."""
        out = tuple(self._catchup.data_ready_datasets)
        if per_contract_rolls(self._settings.config_dir, portfolio_dir) \
                and CONTRACT_DATA_READY_DATASET not in out:
            out += (CONTRACT_DATA_READY_DATASET,)
        return out

    def _ready(self, info: PortfolioInfo) -> Optional[DataReady]:
        """The newest data every dataset of the portfolio has (combine_ready); None while one of
        them has no row at all."""
        rows = [self._store.latest_data_ready(d) for d in self._datasets(info.dir)]
        if any(r is None for r in rows):
            return None
        return combine_ready(rows)

    def _data_verdict(self, info: PortfolioInfo, ready: DataReady,
                      target: dt.date) -> Tuple[Optional[Step], Dict[str, dt.date]]:
        """(None, the missing symbols' last bar dates) when the row can build `target`'s books,
        else (the alert, {}). Partial data is usable when every missing symbol's last bar is
        within the engine's live.data_staleness_tolerance_days of the run's last bar date
        (target - 1, the engine's end_date): the check the engine's freshness and FEED_HOLE
        guards make, done here first so a stale feed builds nothing. The engine still decides
        inside the run; its refusal is an alert like any refused run."""
        parts = ready.parts or (ready,)
        for part in parts:
            if part.status == "failed":
                return Step(ALERT, target, "data_failed",
                            f"data-ngin's {part.dataset} pull for {part.bar_date} failed (run "
                            f"{part.run_id or '-'}, {part.loaded} of {part.expected} loaded): the "
                            f"books of {target} are not built"), {}
            if part.status not in ("complete", "partial"):
                return Step(ALERT, target, "data_failed",
                            f"ops.data_ready {part.dataset} {part.bar_date} has status "
                            f"{part.status!r}: the books of {target} are not built"), {}
        tolerance = staleness_tolerance_days(self._settings.config_dir, info.dir)
        as_of = target - dt.timedelta(days=1)
        last_all: Dict[str, dt.date] = {}
        for part in parts:
            if not part.missing:
                continue
            # The per-contract row names roots: their last contract bar (rolls step 4).
            lookup = (self._store.last_contract_bar_dates
                      if part.dataset == CONTRACT_DATA_READY_DATASET else self._store.last_bar_dates)
            last = lookup(part.missing,
                          part.bar_date - dt.timedelta(days=MISSING_LOOKBACK_DAYS))
            stale = [f"{sym} (last bar {last.get(sym) or 'none'})" for sym in part.missing
                     if last.get(sym) is None or (as_of - last[sym]).days > tolerance]
            if stale:
                return Step(ALERT, target, "data_stale",
                            f"{part.dataset} {part.bar_date} is {part.status} ({part.loaded} of "
                            f"{part.expected}); {len(stale)} missing symbol(s) are past "
                            f"live.data_staleness_tolerance_days={tolerance} as of {as_of}: "
                            f"{', '.join(stale)}. The books of {target} are not built"), {}
            last_all.update(last)
        return None, last_all

    def _morning_gate(self, info: PortfolioInfo, today: dt.date) -> Optional[Step]:
        """None when today's model run may go: data for the previous trading day is ready and
        usable. Not there yet: a wait until QT_DATA_READY_ALERT_AT (06:00; data-ngin's 04:30
        pull may still be running), an alert from then on. A failed or stale row is final: an
        alert at once. The next pass looks again either way."""
        prev = self.calendar.previous_trading_day(today)
        datasets = self._datasets(info.dir)
        rows = {d: self._store.latest_data_ready(d) for d in datasets}
        late = [d for d in datasets if rows[d] is None or rows[d].bar_date < prev]
        if late:
            names = ", ".join(late)
            now_local = self.local_now()
            if now_local.time() < self._catchup.data_alert_at:
                return Step(WAIT_TIME, today, "before_data",
                            f"today's run waits for data-ngin's {names} row for {prev}")
            newest = "; ".join(
                (f"{d}: " if len(late) > 1 else "")
                + (f"newest {rows[d].bar_date} ({rows[d].status})" if rows[d] else "no row at all")
                for d in late)
            return Step(ALERT, today, "data_not_ready",
                        f"no book for {today}, and ops.data_ready has no {names} row for "
                        f"{prev} at {now_local:%H:%M} New York ({newest}): today's model run "
                        "waits for the data")
        bad, _ = self._data_verdict(info, combine_ready([rows[d] for d in datasets]), today)
        return bad

    def _evening_step(self, info: PortfolioInfo, now_local: dt.datetime,
                      last: dt.date) -> Optional[Step]:
        """The next step of the data-driven build of T (a rebuild of today, the review e-mail,
        or the chain up to a later T), or None. Today has its book (last >= today)."""
        today = now_local.date()
        pid = info.portfolio_id
        ready = self._ready(info)
        if ready is None:
            return None
        target = self.calendar.next_trading_day(ready.bar_date)
        if target < today:
            return None   # old data: today's own flow and the data-ready check cover it
        if info.desk_editable and last < target:
            # Dominick 2026-10-18: a day built on the way to the target (Saturday and Sunday on
            # the way to Monday) is reviewed as soon as it is built, and the desk may approve it
            # at once; the next day is built once it is published.
            due = self._review_due(info, now_local, last, ready)
            if due is not None:
                return due
        if info.desk_editable and target > today and self._store.published_at(pid, today) is None:
            return None   # ruling 29: today is approved (or falls back at 10:00) first
        if info.desk_editable and today < last < target \
                and self._store.published_at(pid, last) is None:
            # D-63 (decision 6): every day waits for the desk's approval or its own 10:00
            # fallback, with the alert, weekends included. The engine's chain is every calendar
            # day and starts each from the PUBLISHED day before, so the build stops at the first
            # day after today that is not published yet; it goes on once that day, as today, is
            # approved or falls back. (Before, such a day was published at once with the model's
            # book, system:fallback-nontrading, with no desk window and no alert.)
            return None
        if last < target:
            bad, _ = self._data_verdict(info, ready, target)
            if bad is not None:
                return bad
            day = last + dt.timedelta(days=1)
            return self._blocked(pid, day, RUN) or Step(
                RUN, day, "build",
                f"build of {day} towards {target} from the bars of {ready.bar_date}",
                ready=ready if day == target else None)
        if last > target:
            return None   # a later day exists (run by hand): the target is not rebuilt under it
        if self._store.published_at(pid, target) is not None:
            return None   # published: frozen, never rebuilt (contract C3)
        built = self._store.book_written_at(pid, target, "system")
        if built is None or built < ready.finished_at:
            # Once per data_ready.finished_at: a re-pull (Sunday after Friday) rebuilds the day.
            bad, _ = self._data_verdict(info, ready, target)
            if bad is not None:
                return bad
            return self._blocked(pid, target, RUN) or Step(
                RUN, target, "rebuild",
                f"rebuild of {target}: ops.data_ready {ready.bar_date} finished at "
                f"{ready.finished_at.isoformat()}, after its book ({built})", ready=ready)
        if info.desk_editable:
            return self._review_due(info, now_local, target, ready)
        return None

    def _review_due(self, info: PortfolioInfo, now_local: dt.datetime, day: dt.date,
                    ready: DataReady) -> Optional[Step]:
        """The desk's review e-mail of `day`, when its book is built, unpublished, before its
        10:00 cutoff and not yet reviewed in this build; else None."""
        pid = info.portfolio_id
        if self._store.published_at(pid, day) is not None:
            return None
        built = self._store.book_written_at(pid, day, "system")
        if built is None or now_local >= cutoff.cutoff_time(day) \
                or self.reviews.sent(pid, day, built):
            return None
        return self._blocked(pid, day, REVIEW) or Step(REVIEW, day, "review", ready=ready)

    def _review(self, info: PortfolioInfo, step: Step) -> Optional[Step]:
        """The desk's review e-mail of a day's build (the target or a day on the way to it),
        once per build."""
        pid, target, ready = info.portfolio_id, step.date, step.ready
        built = self._store.book_written_at(pid, target, "system")
        if built is None or ready is None:
            return None
        held_date = self._store.last_published_before(pid, target)
        held = self._store.day_books(pid, held_date).get(info.book, {}) if held_date else {}
        _, last_bars = self._data_verdict(info, ready, target)
        review = review_mail.Review(
            portfolio_id=pid, target=target, bar_date=ready.bar_date, built_at=built,
            books=self._store.day_books(pid, target), held_date=held_date, held=held,
            desk_edits_kept=self._store.desk_edited(pid, target), data_status=ready.status,
            missing=ready.missing, missing_last_bar=last_bars,
            supersedes=self.reviews.earlier(pid, target), desk_url=self._catchup.desk_url)
        subject, body = review_mail.build(review)
        to = list(self._catchup.review_recipients)
        now = self._clock()
        disabled = self._settings.email_disabled
        if disabled:
            self.reviews.log_body(now, to, subject, body)
        else:
            smtp, why = load_smtp(self._settings, info.dir)
            if smtp is None:
                self._failed(pid, target, REVIEW)
                return Step(ALERT, target, "review_mail_failed",
                            f"the review e-mail of {target} cannot be sent: {why}")
            try:
                self._sender(smtp, to, subject, body)
            except Exception as exc:  # smtplib errors can echo the server reply, never a password
                self._failed(pid, target, REVIEW)
                return Step(ALERT, target, "review_mail_failed",
                            f"the review e-mail of {target} failed: {type(exc).__name__}")
        if not self.reviews.record(pid, target, built, now, to, disabled):
            self._failed(pid, target, REVIEW)   # never a loop of e-mails over a broken disk
            return Step(ALERT, target, "review_mail_state",
                        f"the review e-mail of {target} went out but could not be recorded in "
                        f"{review_mail.STATE}; it may be sent again")
        self.files.line(f"{pid}: review e-mail of {target} (bars of {ready.bar_date}"
                        f"{', desk edits kept' if review.desk_edits_kept else ''}"
                        f"{', supersedes ' + review.supersedes if review.supersedes else ''}) "
                        + (f"not sent (QT_EMAIL_DISABLED=1; body in {review_mail.BODY_LOG})"
                           if disabled else f"sent to {', '.join(to)}"))
        return None

    def _data_ready_check(self) -> None:
        """From QT_DATA_READY_ALERT_AT (06:00), every day (Saturday's run needs Friday's data
        too): no non-failed ops.data_ready row for the previous trading day -> one alert to the
        President a day."""
        now_local = self.local_now()
        if now_local.time() < self._catchup.data_alert_at:
            return
        today = now_local.date()
        expected = self.calendar.previous_trading_day(today)
        # Every dataset any portfolio waits for (rolls step 4: the per-contract row too when a
        # portfolio turns per_contract_rolls on), each with its own alert.
        datasets: List[str] = []
        for pdir in self._catchup.portfolios:
            datasets.extend(d for d in self._datasets(pdir) if d not in datasets)
        for dataset in datasets:
            ready = self._store.latest_data_ready(dataset)
            if ready is not None and ready.bar_date >= expected and ready.status != "failed":
                continue
            what = (f"its row is {ready.status}"
                    if ready is not None and ready.bar_date >= expected
                    else f"the newest row is for {ready.bar_date}" if ready is not None
                    else "the table has no row")
            self.alerter.alert(self._catchup.portfolios[0], dataset, expected, "data_not_ready",
                               f"no ready {dataset} data for {expected} at {now_local:%H:%M} New "
                               f"York ({what}): today's model run waits for the data")

    # -- the steps ------------------------------------------------------------------------------

    def _act(self, info: PortfolioInfo, step: Step) -> Optional[Step]:
        """Run one step (the portfolio lock is held). None when it did its work, else the step
        to stop on (an alert or a wait)."""
        if step.action == RUN:
            return self._run_model(info, step.date, step.ready)
        if step.action == FALLBACK:
            return self._fallback(info, step.date, step.reason)
        if step.action == SEND:
            return self._send(info, step.date)
        if step.action == REVIEW:
            return self._review(info, step)
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

    def _run_model(self, info: PortfolioInfo, day: dt.date,
                   ready: Optional[DataReady] = None) -> Optional[Step]:
        """The model run of `day`. `ready`: a data-driven build of the target day from that
        ops.data_ready row (its system book must come out newer than the row)."""
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
            reason = "refused" if res.rc == 2 or any(
                m in detail for m in ("QT_REFUSED", "QT_ALERT", "FEED_HOLE_REFUSAL")) else "failed"
            return Step(ALERT, day, reason, f"the model run exited {res.rc}\n{detail}")
        latest = self._store.last_book_date(info.portfolio_id, info.book)
        if latest is None or latest < day:
            self._failed(info.portfolio_id, day, RUN)
            return Step(ALERT, day, "no_book",
                        f"the model run exited 0 but wrote no {info.book} book for {day}\n"
                        f"{detail}")
        if ready is not None:
            written = self._store.book_written_at(info.portfolio_id, day, "system")
            if written is None or written < ready.finished_at:
                self._failed(info.portfolio_id, day, RUN)
                return Step(ALERT, day, "no_book",
                            f"the build exited 0 but did not rewrite the system book of "
                            f"{day} (written {written}, data finished {ready.finished_at})\n"
                            f"{detail}")
            if info.desk_editable and self._store.desk_edited(info.portfolio_id, day):
                self.files.line(f"{info.portfolio_id}: {day} rebuilt from the bars of "
                                f"{ready.bar_date}; the desk's saved edits were kept (system "
                                "rebuilt, qt_proposal and qt kept)")
        # The twin's day is published when it is today or past: a build ahead of
        # today stays open, so a later re-pull can rebuild it.
        if not info.desk_editable and day <= self.local_now().date() \
                and self._store.publish_model_only(info.portfolio_id, day):
            self.files.line(f"{info.portfolio_id}: {day} published ({cutoff.MODEL_ONLY})")
        return None

    def _fallback(self, info: PortfolioInfo, day: dt.date, kind: str) -> Optional[Step]:
        """Contract C7: qt reset to the model's book and published by the engine (--fallback),
        e-mailed at once for today (10:00) and never for a past day (the catch-up). The
        data-driven run's non-trading day ahead of the target (kind FALLBACK_NONTRADING) is
        published the same way, not e-mailed here (its own 09:30 send does) and not alerted."""
        pid = info.portfolio_id
        send = kind == FALLBACK_TODAY
        requested_by = {FALLBACK_TODAY: cutoff.FALLBACK_10AM,
                        FALLBACK_NONTRADING: cutoff.FALLBACK_NONTRADING}.get(
                            kind, cutoff.FALLBACK_CATCHUP)
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
        if kind == FALLBACK_NONTRADING:
            self.files.line(f"{pid}: {day} (not a trading day) published with the model's book "
                            f"({cutoff.FALLBACK_NONTRADING}, publish row {audit_id})")
            return None
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
            refusal = self._settings_refusal(detail)
            if refusal:  # D-77: name the saved setting that stops the send
                text += f". A saved settings change cannot be applied: {refusal}"
        self.alerter.alert(info.dir, pid, day, "fallback", text + f" (publish row {audit_id})",
                           subject=text)
        return None

    @staticmethod
    def _settings_refusal(detail: str) -> str:
        """D-77: the engine's STRATEGY_CONFIG refusal line (it names the portfolio and why the
        saved settings change cannot be applied), or "" when the output holds none."""
        return next((line.strip() for line in (detail or "").splitlines()
                     if "STRATEGY_CONFIG refused" in line), "")

    def _send(self, info: PortfolioInfo, day: dt.date) -> Optional[Step]:
        argv = [self._settings.engine_binary, "--send", "--portfolio-config", info.dir,
                "--date", day.isoformat()]
        res, detail, _ = self._engine(info, argv, "send", day)
        if res.rc == 0 and self._store.sent_at(info.portfolio_id, day) is not None:
            self.files.line(f"{info.portfolio_id}: {day} e-mailed")
            return None
        self._failed(info.portfolio_id, day, SEND)
        refusal = self._settings_refusal(detail)
        if refusal:
            # D-77: a bad saved setting stops every send until it is fixed; say which.
            return Step(ALERT, day, "send_refused_settings",
                        f"{day} is not e-mailed: a saved settings change cannot be applied. "
                        f"Fix or revert it in the desk's settings; the scheduler retries the "
                        f"send.\n{refusal}")
        why = (res.error and f"cannot start the engine: {res.error}") or (
            res.timed_out and "the send timed out") or (
            f"the send exited {res.rc}" + (" without recording sent_at" if res.rc == 0 else ""))
        return Step(ALERT, day, "send_failed", f"{why}\n{detail}")

    def _missed_send_check(self, info: PortfolioInfo) -> None:
        """D-40: a day that was published (approved, or sent out by its fallback) but never
        e-mailed by the time it became a past day is not sent late (a past day's book must not
        reach the broker as today's); the President is alerted, on each of the
        MISSED_SEND_ALERT_DAYS days after it. A caught-up day is never e-mailed by design and
        was alerted when it was caught up; nothing is alerted while e-mail is off."""
        if self._not_sendable(info):
            return
        today = self.local_now().date()
        since = today - dt.timedelta(days=MISSED_SEND_ALERT_DAYS)
        for day, by in self._store.unsent_days(info.portfolio_id, since, today):
            if by == cutoff.FALLBACK_CATCHUP or self._store.email_held(info.portfolio_id, day):
                continue
            self.alerter.alert(info.dir, info.portfolio_id, day, "missed_send",
                               f"{info.portfolio_id} {day} was published ({by or 'unknown'}) "
                               f"but its daily e-mail was never sent before the day passed: "
                               "it is not sent late; send it by hand if it is still wanted")

    def _sunday_reminder(self, info: PortfolioInfo) -> None:
        """Dominick 2026-10-18: Monday's book is built and approved as early as the data allows
        (Saturday, from Friday's bars). On Sunday from 18:00 New York, the next trading day's
        book not approved yet reminds the QT desk (QT_REVIEW_RECIPIENTS), the VP and the
        President, once."""
        if not info.desk_editable:
            return
        now_local = self.local_now()
        if now_local.weekday() != 6 or now_local.time() < SUNDAY_REMINDER_AT:
            return
        pid = info.portfolio_id
        day = self.calendar.next_trading_day(now_local.date())
        if self._store.published_at(pid, day) is not None:
            return
        try:
            vp = [parse_approvers(self._settings.approvers)["vp"]]
        except ApproverError:
            vp = []
        built = self._store.book_written_at(pid, day, "system") is not None
        state = ("is built and waits for the desk's approval" if built else
                 "is not built yet: the days before it are approved first")
        self.alerter.alert(info.dir, pid, day, "sunday_reminder",
                           f"{pid} {day} {state}. Approve it before "
                           f"{cutoff.SEND_AT:%H:%M} New York on {day}, or the model's book is "
                           f"sent at {cutoff.CUTOFF_AT:%H:%M}.",
                           subject=f"QT reminder: the {pid} book for {day} is not approved yet",
                           also_to=list(self._catchup.review_recipients) + vp)

    def _activity_mail(self) -> None:
        """The VP and President's log (activity_mail.py): one e-mail per desk action and per
        settings version since the last pass, oldest first."""
        state = self.activities.load()
        if state is None:
            # First start: from now; history is not e-mailed.
            if not self.activities.save(self._store.activity_mark()):
                raise OSError(f"cannot write {activity_mail.STATE}")
            return
        try:
            approvers = parse_approvers(self._settings.approvers)
        except ApproverError as exc:
            self.files.line(f"desk-action log: not e-mailed ({exc})")
            return
        to = [approvers["vp"], approvers["president"]]
        before = self._clock() - ACTIVITY_SETTLE
        mark = state
        for row in self._store.desk_commands_after(mark, before, ACTIVITY_BATCH):
            if activity_mail.is_desk_action(row.requested_by):
                subject, body = activity_mail.build_command(row, self._tz)
                if not self._activity_send(to, subject, body):
                    return
            mark = activity_mail.Mark(row.finished_at, row.id, mark.settings_id)
            if not self.activities.save(mark):
                raise OSError(f"cannot write {activity_mail.STATE}")
        for version in self._store.settings_versions_after(mark.settings_id, before,
                                                           ACTIVITY_BATCH):
            if activity_mail.is_desk_action(version.created_by):
                subject, body = activity_mail.build_settings(version, self._tz)
                if not self._activity_send(to, subject, body):
                    return
            mark = activity_mail.Mark(mark.command_at, mark.command_id, version.id)
            if not self.activities.save(mark):
                raise OSError(f"cannot write {activity_mail.STATE}")

    def _activity_send(self, to: List[str], subject: str, body: str) -> bool:
        """True when the e-mail went (or e-mail is off and the body was logged); False: try
        again on the next pass."""
        if self._settings.email_disabled:
            self.activities.log_body(self._clock(), to, subject, body)
            self.files.line(f"desk-action log: {subject} (QT_EMAIL_DISABLED=1; body in "
                            f"{activity_mail.BODY_LOG})")
            return True
        if not self._catchup.portfolios:
            self.files.line("desk-action log: not sent (no portfolio's email.json); next pass")
            return False
        smtp, why = load_smtp(self._settings, self._catchup.portfolios[0])
        if smtp is None:
            self.files.line(f"desk-action log: not sent ({why}); next pass")
            return False
        try:
            self._sender(smtp, to, subject, body)
        except Exception as exc:  # smtplib errors can echo the server reply, never a password
            self.files.line(f"desk-action log: send failed ({type(exc).__name__}); next pass")
            return False
        self.files.line(f"desk-action log: {subject} sent to {', '.join(to)}")
        return True

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
