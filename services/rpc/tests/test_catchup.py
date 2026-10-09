"""The QT scheduler (contracts C6, C7) with a fake clock, a fake engine and an in-memory day
store. 2026-10-09 is a Friday; every calendar day follows the same flow: the model run from
06:45, the desk's approval by 09:30, the 09:30 send, the 10:00 fallback."""

import contextlib
import dataclasses
import datetime as dt
import json
import os
import shutil
import subprocess
import threading

import pytest

from conftest import write_portfolio
from algogators_rpc.services.desk import catchup as cu
from algogators_rpc.services.desk.catchup import (ALERT, FALLBACK, RUN, SEND, UP_TO_DATE,
                                                  WAIT_PUBLISH, WAIT_TIME, CatchupScheduler)
from algogators_rpc.services.desk.config import CatchupSettings, CommandSettings
from algogators_rpc.services.desk.jobs import RunResult

NY = cu._zone("America/New_York")
UTC = dt.timezone.utc
DESK_PID, MODEL_PID = "QT_CONSERVATIVE_PORTFOLIO", "QT_CONSERVATIVE_MODEL_PORTFOLIO"
THU, FRI, SAT, SUN, MON, TUE = (dt.date(2026, 10, d) for d in (8, 9, 10, 11, 12, 13))
BINARY = "/app/build/bin/Release/live_portfolio_conservative"
DESK, TWIN = "qt_conservative", "qt_conservative_model"


def at(day, hh, mm=0):
    return dt.datetime(day.year, day.month, day.day, hh, mm, tzinfo=NY)


class Clock:
    def __init__(self, when):
        self.t = when

    def __call__(self):
        return self.t.astimezone(UTC)


class Days:
    """trading.live_results / live_run_metadata / position_overrides, as the scheduler sees
    them."""

    def __init__(self):
        self.books = {}          # (pid, book) -> {date: written_at}
        self.published = {}      # (pid, date) -> (published_at, published_by, source)
        self.sent = {}           # (pid, date) -> sent_at
        self.rows = {}           # id -> dict(pid, date, requested_by, status, result, message)
        self.open_publish = set()  # (pid, date) with a desk publish row open
        self.error = None

    def write(self, pid, book, day, when):
        self.books.setdefault((pid, book), {})[day] = when

    def last_book_date(self, pid, book):
        if self.error:
            raise RuntimeError(self.error)
        days = self.books.get((pid, book), {})
        return max(days) if days else None

    def published_at(self, pid, day):
        p = self.published.get((pid, day))
        return p[0] if p else None

    def sent_at(self, pid, day):
        return self.sent.get((pid, day))

    def book_written_at(self, pid, day, book):
        return self.books.get((pid, book), {}).get(day)

    def publish_model_only(self, pid, day):
        if (pid, day) in self.published:
            return False
        self.published[(pid, day)] = (dt.datetime.now(UTC), "system:model-only", "model-only")
        return True

    def insert_fallback(self, pid, day, requested_by):
        if (pid, day) in self.open_publish:
            return None
        audit_id = 1000 + len(self.rows)
        self.rows[audit_id] = dict(pid=pid, date=day, requested_by=requested_by,
                                   status="running", result={}, message="")
        return audit_id

    def command_outcome(self, audit_id):
        r = self.rows[audit_id]
        return r["status"], r["result"], r["message"]

    def fail_running(self, audit_id, message):
        r = self.rows[audit_id]
        if r["status"] != "running":
            return False
        r.update(status="failed", message=message)
        return True


class Engine:
    """Plays the engine: the model run writes the books (system; qt too on the desk's
    portfolio) and publishes nothing; --fallback resets qt to the model's book, publishes it and
    sends with --send-now; --send e-mails a published day once."""

    def __init__(self, days, clock):
        self.days, self.clock = days, clock
        self.calls = []
        self.rc = {}             # (mode, dir, date) -> rc to return without doing the work
        self.mails = []          # (pid, date)
        self.resets = []         # (pid, date)
        self.send_fails = False
        self.lock = threading.Lock()

    def run(self, argv, cwd, timeout, copy_to=None):
        with self.lock:
            self.calls.append((list(argv), cwd, timeout))
        d = argv[argv.index("--portfolio-config") + 1]
        day = dt.date.fromisoformat(argv[argv.index("--date") + 1])
        mode = argv[1] if argv[1].startswith("--") and argv[1] != "--portfolio-config" else "model"
        if copy_to is not None:
            copy_to.write(f"engine output for {d} {mode} {day}\n")
        pid = DESK_PID if d == DESK else MODEL_PID
        if (mode, d, day) in self.rc:
            rc = self.rc[(mode, d, day)]
            return RunResult(rc=rc, stdout="", stderr=f"QT_REFUSED feed hole on {day}\n"
                             if rc == 2 else "Segmentation fault\n")
        when = self.clock()
        if mode == "model":
            self.days.write(pid, "system", day, when)
            if d == DESK:
                self.days.write(pid, "qt", day, when)
            return RunResult(rc=0, stdout="done\n")
        if mode == "--fallback":
            audit_id = int(argv[argv.index("--audit-id") + 1])
            row = self.days.rows[audit_id]
            if (pid, day) in self.days.published:
                row.update(status="refused", message=f"{day} was already published at x")
                return RunResult(rc=2)
            self.resets.append((pid, day))
            self.days.published[(pid, day)] = (when, row["requested_by"], "fallback")
            row["result"] = {"published": True, "publish_source": "fallback"}
            if "--send-now" in argv:
                if self.send_fails:
                    row.update(status="done", message="send failed")
                    row["result"].update(emailed=False, send_error="x")
                    return RunResult(rc=1, stderr="QT_SEND the daily e-mail could not be sent\n")
                self.mails.append((pid, day))
                self.days.sent[(pid, day)] = when
                row["result"]["emailed"] = True
            else:
                row["result"].update(emailed=False, send_skipped="past day")
            row["status"] = "done"
            return RunResult(rc=0)
        if mode == "--send":
            if (pid, day) not in self.days.published:
                return RunResult(rc=2, stderr="QT_REFUSED not published\n")
            if (pid, day) in self.days.sent:
                return RunResult(rc=0)
            if self.send_fails:
                return RunResult(rc=1, stderr="smtp down\n")
            self.mails.append((pid, day))
            self.days.sent[(pid, day)] = when
            return RunResult(rc=0)
        raise AssertionError(argv)


@contextlib.contextmanager
def free_lock(path, wait):
    yield True


class Holds:
    """Stands in for the dispatcher's ownership of a fallback row."""

    def __init__(self):
        self.held, self.released = [], []

    def hold(self, audit_id):
        self.held.append(audit_id)
        return True

    def release(self, audit_id):
        self.released.append(audit_id)


SMTP = {"smtp_host": "smtp.x", "username": "bot@x", "password": "pw", "use_tls": False,
        "to_emails": ["desk@x"]}


@pytest.fixture
def env(tmp_path):
    config = tmp_path / "config"
    for d, pid, editable in ((DESK, DESK_PID, True), (TWIN, MODEL_PID, False)):
        p = config / "portfolios" / d
        p.mkdir(parents=True)
        (p / "portfolio.json").write_text(json.dumps({"portfolio_id": pid,
                                                      "qt": {"desk_editable": editable}}))
        (p / "email.json").write_text(json.dumps(SMTP))
    settings = CommandSettings(engine_binary=BINARY, engine_cwd="/app", config_dir=str(config),
                               lock_dir=str(tmp_path / "locks"), log_dir=str(tmp_path / "logs"),
                               job_timeout_s=1800,
                               approvers="vp=vp@algogators.com,president=pres@algogators.com",
                               email_disabled=False)
    clock = Clock(at(FRI, 6, 30))
    days = Days()
    engine = Engine(days, clock)
    sent = []
    holds = Holds()

    def make(portfolios=(DESK, TWIN), lock=free_lock, settings_=None, **catchup):
        return CatchupScheduler(days, settings_ or settings,
                                CatchupSettings(portfolios=portfolios, **catchup),
                                runner=engine, sender=lambda cfg, to, s, b: sent.append((to, s, b)),
                                clock=clock, lock=lock, dispatcher=holds)

    class Env:
        pass
    e = Env()
    e.settings, e.clock, e.days, e.engine, e.sent, e.make, e.config, e.holds = (
        settings, clock, days, engine, sent, make, config, holds)
    e.logs = tmp_path / "logs"
    return e


def calls(env):
    """[(mode, dir, date, send_now)] of the engine runs."""
    out = []
    for argv, _, _ in env.engine.calls:
        mode = argv[1] if argv[1] in ("--fallback", "--send") else "model"
        out.append((mode, argv[argv.index("--portfolio-config") + 1],
                    argv[argv.index("--date") + 1], "--send-now" in argv))
    return out


def alerts(env):
    return [s for _, s, _ in env.sent]


def approve(env, pid, day, when):
    env.days.published[(pid, day)] = (when, "desk@x", "desk")


def model_done(env, day, when=None):
    when = when or at(day, 7)
    for pid, books in ((DESK_PID, ("system", "qt")), (MODEL_PID, ("system",))):
        for b in books:
            env.days.write(pid, b, day, when)


# -- the model run --------------------------------------------------------------------------------

def test_engine_argv_matches_qt_model_run_sh(env):
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    env.days.published[(MODEL_PID, THU)] = (at(THU, 7), "system:model-only", "model-only")
    env.clock.t = at(FRI, 6, 45)
    env.make(portfolios=(TWIN,)).run_pass()
    argv, cwd, timeout = env.engine.calls[0]
    assert argv == [BINARY, "--portfolio-config", TWIN, "--date", "2026-10-09"]
    assert cwd == "/app" and timeout == 1800


def test_todays_model_run_starts_at_0645_every_day(env):
    """No trading calendar any more: Saturday's book runs at 06:45 like Friday's."""
    env.days.write(DESK_PID, "qt", FRI, at(FRI, 7))
    approve(env, DESK_PID, FRI, at(FRI, 9))
    env.days.sent[(DESK_PID, FRI)] = at(FRI, 9, 30)
    sched = env.make(portfolios=(DESK,))
    env.clock.t = at(SAT, 6, 40)
    assert sched.catch_up(DESK).action == WAIT_TIME and env.engine.calls == []
    env.clock.t = at(SAT, 6, 45)
    step = sched.catch_up(DESK)
    assert calls(env) == [("model", DESK, "2026-10-10", False)]
    assert step.action == WAIT_PUBLISH and step.date == SAT       # never auto-published
    assert env.days.published_at(DESK_PID, SAT) is None


def test_a_failed_model_run_is_retried_every_15_minutes_and_alerts_at_0830(env):
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    env.engine.rc[("model", TWIN, FRI)] = 1
    sched = env.make(portfolios=(TWIN,))
    for hh, mm in ((6, 45), (6, 50), (6, 55), (7, 0), (7, 5)):
        env.clock.t = at(FRI, hh, mm)
        sched.run_pass()
    assert [c[2] for c in calls(env)] == ["2026-10-09", "2026-10-09"]   # 06:45 and 07:00
    assert alerts(env) == [f"QT alert: {MODEL_PID} 2026-10-09 failed"]   # once a day
    env.clock.t = at(FRI, 8, 25)
    sched.run_pass()
    assert f"QT alert: {MODEL_PID} 2026-10-09 model_not_done" not in alerts(env)
    env.clock.t = at(FRI, 8, 30)
    sched.run_pass()
    assert alerts(env)[-1] == f"QT alert: {MODEL_PID} 2026-10-09 model_not_done"
    del env.engine.rc[("model", TWIN, FRI)]
    env.clock.t = at(FRI, 8, 45)
    sched.run_pass()
    assert env.days.last_book_date(MODEL_PID, "system") == FRI
    assert env.days.published[(MODEL_PID, FRI)][2] == "model-only"


def test_no_model_book_alert_at_0830_once(env):
    env.days.write(DESK_PID, "qt", THU, at(THU, 7))
    approve(env, DESK_PID, THU, at(THU, 9))
    env.engine.rc[("model", DESK, FRI)] = 2
    sched = env.make(portfolios=(DESK,))
    env.clock.t = at(FRI, 8, 30)
    sched.run_pass()
    env.clock.t = at(FRI, 8, 35)
    sched.run_pass()
    assert alerts(env).count(f"QT alert: {DESK_PID} 2026-10-09 model_not_done") == 1
    assert f"QT alert: {DESK_PID} 2026-10-09 refused" in alerts(env)


# -- the 09:30 send -------------------------------------------------------------------------------

@pytest.mark.parametrize("hh,mm,sent", [(9, 29, False), (9, 30, True), (9, 59, True)])
def test_an_approved_day_is_sent_at_0930(env, hh, mm, sent):
    model_done(env, FRI)
    approve(env, DESK_PID, FRI, at(FRI, 8))
    env.clock.t = at(FRI, hh, mm)
    env.make(portfolios=(DESK,)).run_pass()
    assert env.engine.mails == ([(DESK_PID, FRI)] if sent else [])
    assert (("--send", DESK, "2026-10-09", False) in calls(env)) is sent


def test_the_send_happens_exactly_once(env):
    model_done(env, FRI)
    approve(env, DESK_PID, FRI, at(FRI, 8))
    sched = env.make(portfolios=(DESK,))
    for hh, mm in ((9, 30), (9, 35), (9, 40), (10, 0), (10, 7), (11, 0)):
        env.clock.t = at(FRI, hh, mm)
        sched.run_pass()
    assert env.engine.mails == [(DESK_PID, FRI)]
    assert [c[0] for c in calls(env)] == ["--send"]


def test_a_failed_send_alerts_and_is_retried(env):
    model_done(env, FRI)
    approve(env, DESK_PID, FRI, at(FRI, 8))
    env.engine.send_fails = True
    sched = env.make(portfolios=(DESK,))
    env.clock.t = at(FRI, 9, 30)
    out = dict(sched.run_pass())
    assert out[DESK].action == ALERT and out[DESK].reason == "send_failed"
    env.clock.t = at(FRI, 9, 35)
    sched.run_pass()                                           # waits 15 min
    env.engine.send_fails = False
    env.clock.t = at(FRI, 9, 45)
    sched.run_pass()
    assert env.engine.mails == [(DESK_PID, FRI)] and len(calls(env)) == 2


def test_nothing_is_sent_with_email_disabled_or_no_recipients(env):
    model_done(env, FRI)
    approve(env, DESK_PID, FRI, at(FRI, 8))
    env.clock.t = at(FRI, 9, 30)
    off = dataclasses.replace(env.settings, email_disabled=True)
    step = env.make(portfolios=(DESK,), settings_=off).catch_up(DESK)
    assert step.reason == "not_sent" and env.engine.calls == []
    (env.config / "portfolios" / DESK / "email.json").write_text(
        json.dumps(dict(SMTP, to_emails=[])))
    step = env.make(portfolios=(DESK,)).catch_up(DESK)
    assert step.reason == "not_sent" and "no recipients" in step.message
    assert env.engine.calls == []


# -- the 10:00 fallback ---------------------------------------------------------------------------

@pytest.mark.parametrize("hh,mm,falls_back", [(9, 59, False), (10, 0, True), (10, 7, True)])
def test_an_unapproved_day_falls_back_at_1000(env, hh, mm, falls_back):
    """10:07: a pass that ran late (a restart, a missed tick) still does the fallback."""
    model_done(env, FRI)
    env.clock.t = at(FRI, hh, mm)
    step = env.make(portfolios=(DESK,)).catch_up(DESK)
    if not falls_back:
        assert step.action == WAIT_PUBLISH and env.engine.calls == []
        return
    assert calls(env) == [("--fallback", DESK, "2026-10-09", True)]
    row = next(iter(env.days.rows.values()))
    assert row["requested_by"] == "system:fallback-10am" and row["status"] == "done"
    assert env.days.published[(DESK_PID, FRI)][1:] == ("system:fallback-10am", "fallback")
    assert env.engine.resets == [(DESK_PID, FRI)] and env.engine.mails == [(DESK_PID, FRI)]
    subject = f"QT did not approve {DESK_PID} 2026-10-09; the model's book was sent at " \
              f"{hh:02d}:{mm:02d}"
    assert alerts(env) == [subject]
    assert env.sent[0][0] == ["pres@algogators.com"]
    assert env.holds.held == env.holds.released == list(env.days.rows)
    assert step.action == UP_TO_DATE


def test_the_fallback_resets_a_desk_save_and_an_override(env):
    """A desk save and an approved override that were never approved as the day's book: the
    fallback resets qt to the model's book (the engine's --fallback; the reset itself is
    tested in C++ and qt_runner_check.sh)."""
    model_done(env, FRI)
    env.days.write(DESK_PID, "qt", FRI, at(FRI, 8, 15))      # a desk save rewrote qt
    env.clock.t = at(FRI, 10, 0)
    env.make(portfolios=(DESK,)).run_pass()
    assert env.engine.resets == [(DESK_PID, FRI)]
    assert env.days.published[(DESK_PID, FRI)][2] == "fallback"


def test_an_approved_day_never_falls_back(env):
    model_done(env, FRI)
    approve(env, DESK_PID, FRI, at(FRI, 9, 59))
    env.days.sent[(DESK_PID, FRI)] = at(FRI, 9, 59)
    env.clock.t = at(FRI, 10, 0)
    assert env.make(portfolios=(DESK,)).catch_up(DESK).action == UP_TO_DATE
    assert env.engine.calls == [] and env.sent == []


def test_an_open_desk_approval_makes_the_fallback_wait(env):
    model_done(env, FRI)
    env.days.open_publish.add((DESK_PID, FRI))
    env.clock.t = at(FRI, 10, 0)
    step = env.make(portfolios=(DESK,)).catch_up(DESK)
    assert step.action == WAIT_TIME and step.reason == "publish_open"
    assert env.engine.calls == [] and env.sent == []


def test_a_fallback_whose_send_fails_alerts_and_the_send_is_retried(env):
    model_done(env, FRI)
    env.engine.send_fails = True
    sched = env.make(portfolios=(DESK,))
    env.clock.t = at(FRI, 10, 0)
    sched.run_pass()
    assert "the e-mail could not be sent" in alerts(env)[0]
    env.engine.send_fails = False
    env.clock.t = at(FRI, 10, 15)
    sched.run_pass()
    assert env.engine.mails == [(DESK_PID, FRI)]
    assert [c[0] for c in calls(env)] == ["--fallback", "--send"]


def test_a_failed_fallback_alerts_and_is_retried(env):
    model_done(env, FRI)
    env.engine.rc[("--fallback", DESK, FRI)] = 1
    sched = env.make(portfolios=(DESK,))
    env.clock.t = at(FRI, 10, 0)
    out = dict(sched.run_pass())
    row = next(iter(env.days.rows.values()))
    assert row["status"] == "failed" and "without recording an outcome" in row["message"]
    assert out[DESK].action == ALERT and out[DESK].reason == "fallback_failed"
    del env.engine.rc[("--fallback", DESK, FRI)]
    env.clock.t = at(FRI, 10, 5)
    sched.run_pass()
    assert len(calls(env)) == 1                                # waits 15 min
    env.clock.t = at(FRI, 10, 15)
    sched.run_pass()
    assert env.days.published[(DESK_PID, FRI)][2] == "fallback"


@pytest.mark.parametrize("day", [SAT, SUN])
def test_a_weekend_day_follows_the_same_flow(env, day):
    prev = day - dt.timedelta(days=1)
    env.days.write(DESK_PID, "qt", prev, at(prev, 7))
    approve(env, DESK_PID, prev, at(prev, 9))
    env.days.sent[(DESK_PID, prev)] = at(prev, 9, 30)
    sched = env.make(portfolios=(DESK,))
    env.clock.t = at(day, 6, 45)
    sched.run_pass()
    assert env.days.published_at(DESK_PID, day) is None       # no silent auto-publish
    env.clock.t = at(day, 10, 0)
    sched.run_pass()
    assert calls(env) == [("model", DESK, day.isoformat(), False),
                          ("--fallback", DESK, day.isoformat(), True)]
    assert env.engine.mails == [(DESK_PID, day)]


@pytest.mark.parametrize("day,utc_hh", [(dt.date(2026, 11, 1), 15),    # EST from 02:00
                                        (dt.date(2026, 3, 8), 14)])    # EDT from 02:00
def test_the_cutoff_is_new_york_time_on_a_dst_day(env, day, utc_hh):
    model_done(env, day)
    sched = env.make(portfolios=(DESK,))
    env.clock.t = dt.datetime(day.year, day.month, day.day, utc_hh - 1, 59, tzinfo=UTC)
    assert env.clock.t.astimezone(NY).strftime("%H:%M") == "09:59"
    assert sched.catch_up(DESK).action == WAIT_PUBLISH
    env.clock.t = dt.datetime(day.year, day.month, day.day, utc_hh, 0, tzinfo=UTC)
    sched.catch_up(DESK)
    assert calls(env) == [("--fallback", DESK, day.isoformat(), True)]


# -- the catch-up of missed past days ------------------------------------------------------------

def test_missed_past_days_are_caught_up_without_an_email(env):
    """The engine was down from Thursday 09:00 to Saturday 07:00: Thursday's book was never
    approved, Friday never ran. Both are published with the model's book, not e-mailed, and
    alerted once; Saturday then runs."""
    env.days.write(DESK_PID, "qt", THU, at(THU, 7))
    env.clock.t = at(SAT, 7, 0)
    out = dict(env.make(portfolios=(DESK,)).run_pass())
    assert calls(env) == [("--fallback", DESK, "2026-10-08", False),
                          ("model", DESK, "2026-10-09", False),
                          ("--fallback", DESK, "2026-10-09", False),
                          ("model", DESK, "2026-10-10", False)]
    assert [r["requested_by"] for r in env.days.rows.values()] == ["system:fallback-catchup"] * 2
    assert all(r["result"]["send_skipped"] == "past day" for r in env.days.rows.values())
    assert env.engine.mails == [] and env.days.sent == {}
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-08 caught_up",
                           f"QT alert: {DESK_PID} 2026-10-09 caught_up"]
    assert out[DESK].action == WAIT_PUBLISH and out[DESK].date == SAT


def test_a_past_approved_but_unsent_day_is_not_emailed(env):
    env.days.write(DESK_PID, "qt", THU, at(THU, 7))
    approve(env, DESK_PID, THU, at(THU, 9))
    env.clock.t = at(FRI, 9, 30)
    env.make(portfolios=(DESK,)).run_pass()
    assert calls(env) == [("model", DESK, "2026-10-09", False)] and env.engine.mails == []


def test_first_desk_day_starts_after_the_system_book(env):
    env.days.write(DESK_PID, "system", THU, at(THU, 7))       # no qt book yet (ruling 27)
    env.clock.t = at(FRI, 7)
    env.make(portfolios=(DESK,)).run_pass()
    assert calls(env) == [("model", DESK, "2026-10-09", False)]


# -- the model twin -------------------------------------------------------------------------------

def test_the_model_twin_publishes_model_only_and_is_sent_at_0930(env):
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    env.days.published[(MODEL_PID, THU)] = (at(THU, 7), "system:model-only", "model-only")
    sched = env.make(portfolios=(TWIN,))
    env.clock.t = at(FRI, 6, 45)
    sched.run_pass()
    assert env.days.published[(MODEL_PID, FRI)][1:] == ("system:model-only", "model-only")
    assert env.engine.mails == []
    env.clock.t = at(FRI, 9, 30)
    sched.run_pass()
    assert env.engine.mails == [(MODEL_PID, FRI)]
    assert [c[0] for c in calls(env)] == ["model", "--send"]
    assert not env.days.rows                                   # never a fallback


def test_the_model_twin_without_recipients_is_not_sent(env):
    (env.config / "portfolios" / TWIN / "email.json").write_text(
        json.dumps(dict(SMTP, to_emails=[])))
    model_done(env, FRI)
    env.clock.t = at(FRI, 9, 30)
    step = env.make(portfolios=(TWIN,)).catch_up(TWIN)
    assert env.days.published[(MODEL_PID, FRI)][2] == "model-only"
    assert step.reason == "not_sent" and env.engine.calls == []


def test_the_model_twins_past_days_are_published_but_not_sent(env):
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    env.clock.t = at(SAT, 9, 45)
    env.make(portfolios=(TWIN,)).run_pass()
    assert [c[2] for c in calls(env) if c[0] == "model"] == ["2026-10-09", "2026-10-10"]
    assert env.days.published_at(MODEL_PID, FRI) is not None
    assert env.engine.mails == [(MODEL_PID, SAT)]


# -- failures, alerts, locks ---------------------------------------------------------------------

def test_refusal_reason_and_distinct_reasons_alert_separately(env):
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    env.engine.rc[("model", TWIN, FRI)] = 2
    sched = env.make(portfolios=(TWIN,))
    env.clock.t = at(FRI, 7)
    sched.run_pass()
    env.engine.rc[("model", TWIN, FRI)] = 1
    env.clock.t = at(FRI, 7, 15)
    sched.run_pass()
    assert alerts(env) == [f"QT alert: {MODEL_PID} 2026-10-09 refused",
                           f"QT alert: {MODEL_PID} 2026-10-09 failed"]
    log = (env.logs / cu.CATCHUP_LOG).read_text()
    assert "e-mailed to the president" in log
    assert "engine output for qt_conservative_model model 2026-10-09" in \
        (env.logs / cu.ENGINE_LOG).read_text()


def test_alerts_respect_qt_email_disabled(env):
    off = dataclasses.replace(env.settings, email_disabled=True)
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    env.engine.rc[("model", TWIN, FRI)] = 1
    env.clock.t = at(FRI, 7)
    env.make(portfolios=(TWIN,), settings_=off).run_pass()
    assert env.sent == []
    assert "not e-mailed (QT_EMAIL_DISABLED=1)" in (env.logs / cu.CATCHUP_LOG).read_text()


def test_exit_0_without_a_book_is_an_alert(env):
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    env.engine.run = lambda argv, cwd, timeout, copy_to=None: RunResult(rc=0)
    env.clock.t = at(FRI, 7)
    step = env.make(portfolios=(TWIN,)).catch_up(TWIN)
    assert step.action == ALERT and step.reason == "no_book"


def test_bad_config_is_an_alert(env):
    out = dict(CatchupScheduler(env.days, env.settings, CatchupSettings(portfolios=("nope",)),
                                runner=env.engine, clock=env.clock, lock=free_lock).run_pass())
    assert out["nope"].reason == "config" and env.engine.calls == []


def test_a_held_portfolio_lock_skips_the_portfolio(env):
    @contextlib.contextmanager
    def busy(path, wait):
        yield not path.endswith(f"{MODEL_PID}.lock")
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    env.clock.t = at(FRI, 7)
    out = dict(env.make(portfolios=(TWIN,), lock=busy).run_pass())
    assert out[TWIN].reason == "busy" and env.engine.calls == []


def test_a_held_scheduler_lock_skips_the_pass(env):
    @contextlib.contextmanager
    def held(path, wait):
        yield not path.endswith(cu.SCHEDULER_LOCK)
    assert env.make(lock=held).run_pass() is None


def test_one_failing_portfolio_does_not_stop_the_others(env):
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    real = env.days.last_book_date

    def flaky(pid, book):
        if pid == DESK_PID:
            raise RuntimeError("database error (OperationalError)")
        return real(pid, book)
    env.days.last_book_date = flaky
    env.clock.t = at(FRI, 7)
    out = dict(env.make().run_pass())
    assert out[DESK].reason == "scheduler_error"
    assert calls(env) == [("model", TWIN, "2026-10-09", False)]


def test_the_unpublished_reminder_is_gone(env):
    model_done(env, FRI)
    approve(env, DESK_PID, FRI, at(FRI, 9))
    env.days.sent[(DESK_PID, FRI)] = at(FRI, 9, 30)
    env.clock.t = at(SAT, 6, 40)
    env.make(portfolios=(DESK,)).run_pass()
    assert env.sent == []
    assert not hasattr(CatchupSettings(), "reminder_after_h")


# -- when -----------------------------------------------------------------------------------------

def test_tick_every_5_minutes_until_1030_then_every_30(env):
    sched = env.make(portfolios=(TWIN,))
    ran = []
    sched.run_pass = lambda: ran.append(sched.local_now().strftime("%H:%M"))
    for hh, mm in ((6, 29), (6, 30), (6, 31), (6, 35), (9, 29), (9, 30), (9, 34), (10, 0),
                   (10, 7), (10, 29), (10, 30), (10, 45), (11, 0), (21, 59), (22, 0), (22, 1)):
        env.clock.t = at(FRI, hh, mm)
        sched.tick()
    assert ran == ["06:30", "06:35", "09:29", "09:30", "10:00", "10:07", "10:29", "10:30",
                   "11:00", "21:59", "22:00"]


def test_tick_disabled(env):
    sched = env.make(enabled=False)
    sched.run_pass = lambda: pytest.fail("ran")
    assert sched.tick() is False


def test_tick_is_on_new_york_time_across_dst(env):
    sched = env.make()
    # 2026-11-01: EDT -> EST. 10:30 UTC is 06:30 EDT on Oct 31 and 05:30 EST on Nov 1.
    assert sched.in_window(dt.datetime(2026, 10, 31, 10, 30, tzinfo=UTC).astimezone(NY))
    env.clock.t = dt.datetime(2026, 11, 1, 10, 30, tzinfo=UTC)
    assert not sched.in_window(sched.local_now())


# -- the watchdog CLI -----------------------------------------------------------------------------

def test_watchdog_runs_a_pass_only_when_the_scheduler_is_late(env, capsys):
    model_done(env, FRI)
    sched = env.make(portfolios=(TWIN,))
    env.clock.t = at(FRI, 8, 0)
    assert cu.main(["watchdog"], scheduler=sched) == 0
    assert "running a pass" in capsys.readouterr().out
    assert sched.heartbeat_age_s() == 0
    env.clock.t = at(FRI, 8, 14)                               # busy window: 15 min
    assert cu.main(["watchdog"], scheduler=sched) == 0
    assert "ok (last pass 14 min ago)" in capsys.readouterr().out
    env.clock.t = at(FRI, 8, 16)
    cu.main(["watchdog"], scheduler=sched)
    assert "running a pass" in capsys.readouterr().out
    env.clock.t = at(FRI, 12, 0)
    cu.main(["watchdog"], scheduler=sched)                     # runs (3 h 44 min)
    capsys.readouterr()
    env.clock.t = at(FRI, 12, 44)                              # after 10:30: 75 min
    cu.main(["watchdog"], scheduler=sched)
    assert "ok (last pass 44 min ago)" in capsys.readouterr().out
    env.clock.t = at(FRI, 23, 0)
    cu.main(["watchdog"], scheduler=sched)
    assert "outside the window" in capsys.readouterr().out
    assert cu.main(["bogus"], scheduler=sched) == 2


def test_run_once_exit_status_reports_alerts(env):
    env.days.write(MODEL_PID, "system", THU, at(THU, 7))
    env.engine.rc[("model", TWIN, FRI)] = 1
    env.clock.t = at(FRI, 7)
    assert cu.main(["run-once"], scheduler=env.make(portfolios=(TWIN,))) == 1


# -- the real flock -------------------------------------------------------------------------------

@pytest.mark.skipif(os.name != "posix" or shutil.which("flock") is None, reason="needs flock(1)")
def test_file_lock_is_the_lock_flock1_takes(tmp_path):
    path = str(tmp_path / "locks" / "P.lock")
    with cu.file_lock(path, 0) as held:
        assert held
        busy = subprocess.run(["flock", "-n", path, "true"])
        assert busy.returncode != 0                    # a desk job would wait
        with cu.file_lock(path, 0.2, poll_s=0.05) as again:
            assert again is False
    assert subprocess.run(["flock", "-n", path, "true"]).returncode == 0
    holder = subprocess.Popen(["flock", path, "sleep", "1"])
    try:
        import time
        time.sleep(0.2)
        with cu.file_lock(path, 0) as held:
            assert held is False
        with cu.file_lock(path, 5, poll_s=0.05) as held:
            assert held                                # waits for the desk job to finish
    finally:
        holder.wait()


def test_with_smtp_helper_kept_for_other_tests(env):
    write_portfolio(env.config, dirname="x", portfolio_id="X", email=SMTP)
    assert cu.recipients(str(env.config), "x") == ["desk@x"]
    assert cu.recipients(str(env.config), "missing") == []
