"""The catch-up scheduler (contract C6) with a fake clock, a fake engine and an in-memory day
store. Calendar: weekends closed plus the holidays given; 2026-10-09 is a Friday."""

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
from algogators_rpc.services.desk.catchup import (ALERT, RUN, UP_TO_DATE, WAIT_PUBLISH, WAIT_TIME,
                                                  Calendar, CalendarError, CatchupScheduler)
from algogators_rpc.services.desk.config import CatchupSettings, CommandSettings
from algogators_rpc.services.desk.jobs import RunResult

NY = cu._zone("America/New_York")
DESK_PID, MODEL_PID = "QT_CONSERVATIVE_PORTFOLIO", "QT_CONSERVATIVE_MODEL_PORTFOLIO"
FRI, SAT, SUN, MON, TUE = (dt.date(2026, 10, d) for d in (9, 10, 11, 12, 13))
BINARY = "/app/build/bin/Release/live_portfolio_conservative"


def at(day, hh, mm=0):
    return dt.datetime(day.year, day.month, day.day, hh, mm, tzinfo=NY)


class Clock:
    def __init__(self, when):
        self.t = when

    def __call__(self):
        return self.t.astimezone(dt.timezone.utc)


class Days:
    """trading.live_results / live_run_metadata, as the scheduler reads them."""

    def __init__(self):
        self.books = {}          # (pid, book) -> {date: written_at}
        self.published = {}      # (pid, date) -> published_at
        self.error = None

    def write(self, pid, book, day, when):
        self.books.setdefault((pid, book), {})[day] = when

    def last_book_date(self, pid, book):
        if self.error:
            raise RuntimeError(self.error)
        days = self.books.get((pid, book), {})
        return max(days) if days else None

    def published_at(self, pid, day):
        return self.published.get((pid, day))

    def book_written_at(self, pid, day, book):
        return self.books.get((pid, book), {}).get(day)


class Engine:
    """Plays the model run: writes the books for --date and auto-publishes a non-trading day
    of a desk-editable portfolio, as the engine does (C5)."""

    def __init__(self, days, clock, calendar, editable):
        self.days, self.clock, self.calendar, self.editable = days, clock, calendar, editable
        self.calls = []
        self.rc = {}             # (dir, date) -> rc to return without writing
        self.lock = threading.Lock()

    def run(self, argv, cwd, timeout, copy_to=None):
        with self.lock:
            self.calls.append((list(argv), cwd, timeout))
        d = argv[argv.index("--portfolio-config") + 1]
        day = dt.date.fromisoformat(argv[argv.index("--date") + 1])
        if copy_to is not None:
            copy_to.write(f"engine output for {d} {day}\n")
        if (d, day) in self.rc:
            rc = self.rc[(d, day)]
            return RunResult(rc=rc, stdout="", stderr=f"QT_REFUSED feed hole on {day}\n"
                             if rc == 2 else "Segmentation fault\n")
        pid = DESK_PID if d == "qt_conservative" else MODEL_PID
        when = self.clock()
        self.days.write(pid, "system", day, when)
        if self.editable.get(d):
            self.days.write(pid, "qt", day, when)
            if not self.calendar.is_trading_day(day):
                self.days.published[(pid, day)] = when
        return RunResult(rc=0, stdout="done\n")


@contextlib.contextmanager
def free_lock(path, wait):
    yield True


@pytest.fixture
def cal():
    return Calendar(holidays={dt.date(2026, 11, 26)}, years={2026})


@pytest.fixture
def env(tmp_path, cal):
    config = tmp_path / "config"
    for d, pid, editable in (("qt_conservative", DESK_PID, True),
                             ("qt_conservative_model", MODEL_PID, False)):
        p = config / "portfolios" / d
        p.mkdir(parents=True)
        (p / "portfolio.json").write_text(json.dumps({"portfolio_id": pid,
                                                      "qt": {"desk_editable": editable}}))
    settings = CommandSettings(engine_binary=BINARY, engine_cwd="/app", config_dir=str(config),
                               lock_dir=str(tmp_path / "locks"), log_dir=str(tmp_path / "logs"),
                               job_timeout_s=1800,
                               approvers="vp=vp@algogators.com,president=pres@algogators.com",
                               email_disabled=False)
    clock = Clock(at(MON, 8))
    days = Days()
    engine = Engine(days, clock, cal, {"qt_conservative": True})
    sent = []

    def make(portfolios=("qt_conservative", "qt_conservative_model"), lock=free_lock,
             **catchup):
        return CatchupScheduler(days, settings, CatchupSettings(portfolios=portfolios, **catchup),
                                runner=engine, sender=lambda cfg, to, s, b: sent.append((to, s, b)),
                                clock=clock, calendar_loader=lambda: cal, lock=lock)

    class Env:
        pass
    e = Env()
    e.settings, e.clock, e.days, e.engine, e.sent, e.make, e.config = (
        settings, clock, days, engine, sent, make, config)
    e.logs = tmp_path / "logs"
    return e


def with_smtp(env):
    for d in ("qt_conservative", "qt_conservative_model"):
        write_portfolio(env.config, dirname=d,
                        portfolio_id=DESK_PID if d == "qt_conservative" else MODEL_PID,
                        email={"smtp_host": "smtp.x", "username": "bot@x", "password": "pw",
                               "use_tls": False})
        pj = env.config / "portfolios" / d / "portfolio.json"
        data = json.loads(pj.read_text())
        data["qt"] = {"desk_editable": d == "qt_conservative"}
        pj.write_text(json.dumps(data))


def dates_run(env):
    return [(c[0][c[0].index("--portfolio-config") + 1], c[0][-1]) for c in env.engine.calls]


# -- the calendar ---------------------------------------------------------------------------------

def test_calendar_trading_days(cal):
    assert cal.is_trading_day(SAT)           # T-1 Friday was open: the desk publishes Saturday
    assert not cal.is_trading_day(SUN)       # T-1 Saturday
    assert not cal.is_trading_day(MON)       # T-1 Sunday
    assert cal.is_trading_day(TUE)
    assert not cal.is_trading_day(dt.date(2026, 11, 27))   # T-1 Thanksgiving
    with pytest.raises(CalendarError):
        cal.is_trading_day(dt.date(2027, 1, 5))


def test_calendar_loads_holidays_json(tmp_path):
    path = tmp_path / "holidays.json"
    path.write_text(json.dumps({"2026": [{"date": "2026-11-26", "name": "Thanksgiving"}]}))
    cal = Calendar.load(str(path))
    assert not cal.is_session(dt.date(2026, 11, 26)) and cal.is_session(dt.date(2026, 11, 25))
    with pytest.raises(CalendarError):
        Calendar.load(str(tmp_path / "missing.json"))


def test_holidays_path_resolution(tmp_path):
    (tmp_path / "include/trade_ngin/core").mkdir(parents=True)
    (tmp_path / "include/trade_ngin/core/holidays.json").write_text("{}")
    assert cu.holidays_path(str(tmp_path), {}) == str(tmp_path / "include/trade_ngin/core"
                                                      / "holidays.json")
    assert cu.holidays_path(str(tmp_path), {"TRADE_NGIN_HOLIDAYS_JSON": "/x.json"}) == "/x.json"


# -- the walk -------------------------------------------------------------------------------------

def test_weekend_chain_runs_once_saturday_is_published(env):
    """C6: the desk publishes Saturday's book on Monday morning; Sunday and Monday then run in
    the next pass (both non-trading, so auto-published and not held for 10:15)."""
    env.days.write(DESK_PID, "qt", SAT, at(SAT, 10, 30))
    env.days.write(MODEL_PID, "system", SAT, at(SAT, 10, 30))
    sched = env.make()
    sched.run_pass()
    assert dates_run(env) == [("qt_conservative_model", "2026-10-11"),
                              ("qt_conservative_model", "2026-10-12")]   # desk waits
    env.days.published[(DESK_PID, SAT)] = at(MON, 7, 40)
    env.clock.t = at(MON, 8, 0)
    out = dict(sched.run_pass())
    assert dates_run(env)[2:] == [("qt_conservative", "2026-10-11"),
                                  ("qt_conservative", "2026-10-12")]
    assert out["qt_conservative"].action == UP_TO_DATE
    assert env.sent == []


def test_engine_argv_matches_qt_model_run_sh(env):
    env.days.write(MODEL_PID, "system", SUN, at(SUN, 10))
    env.make(portfolios=("qt_conservative_model",)).run_pass()
    argv, cwd, timeout = env.engine.calls[0]
    assert argv == [BINARY, "--portfolio-config", "qt_conservative_model", "--date", "2026-10-12"]
    assert cwd == "/app" and timeout == 1800


def test_trading_day_waits_for_its_publish_without_alert(env):
    env.days.write(DESK_PID, "qt", FRI, at(FRI, 10, 30))      # Friday: T-1 Thursday, trading
    env.clock.t = at(SAT, 9)
    step = env.make(portfolios=("qt_conservative",)).catch_up("qt_conservative")
    assert step.action == WAIT_PUBLISH and step.date == FRI
    assert env.engine.calls == [] and env.sent == []


def test_reminder_after_24h_once_a_day(env):
    with_smtp(env)
    env.days.write(DESK_PID, "qt", FRI, at(FRI, 10, 30))
    sched = env.make(portfolios=("qt_conservative",))
    env.clock.t = at(SAT, 10)                                  # 23.5 h
    sched.run_pass()
    assert env.sent == []
    env.clock.t = at(SAT, 11)                                  # 24.5 h
    sched.run_pass()
    env.clock.t = at(SAT, 11, 30)
    sched.run_pass()
    assert len(env.sent) == 1
    to, subject, body = env.sent[0]
    assert to == ["pres@algogators.com"]
    assert subject == f"QT alert: {DESK_PID} 2026-10-09 unpublished_reminder"
    env.clock.t = at(SUN, 6)                                   # a new day: once more
    sched.run_pass()
    assert len(env.sent) == 2 and env.engine.calls == []


def test_today_on_a_trading_day_waits_for_10_15(env):
    env.days.write(DESK_PID, "qt", MON, at(MON, 10))
    env.days.published[(DESK_PID, MON)] = at(MON, 11)
    sched = env.make(portfolios=("qt_conservative",))
    env.clock.t = at(TUE, 9, 30)
    assert sched.catch_up("qt_conservative").action == WAIT_TIME
    assert env.engine.calls == []
    env.clock.t = at(TUE, 10, 30)
    step = sched.catch_up("qt_conservative")
    assert dates_run(env) == [("qt_conservative", "2026-10-13")]
    assert step.action == UP_TO_DATE


def test_days_before_today_run_at_once(env):
    env.days.write(MODEL_PID, "system", FRI, at(FRI, 10))
    env.clock.t = at(TUE, 6, 0)
    env.make(portfolios=("qt_conservative_model",)).run_pass()
    # Saturday..Monday run; Tuesday (trading, today) waits for 10:15.
    assert [d for _, d in dates_run(env)] == ["2026-10-10", "2026-10-11", "2026-10-12"]


def test_model_twin_is_never_held_for_a_publish(env):
    env.days.write(MODEL_PID, "system", FRI, at(FRI, 10))
    env.clock.t = at(SAT, 10, 30)
    env.make(portfolios=("qt_conservative_model",)).run_pass()
    assert dates_run(env) == [("qt_conservative_model", "2026-10-10")]


def test_first_desk_day_starts_after_the_system_book(env):
    env.days.write(DESK_PID, "system", SUN, at(SUN, 10))       # no qt book yet (ruling 27)
    env.make(portfolios=("qt_conservative",)).run_pass()
    assert dates_run(env) == [("qt_conservative", "2026-10-12")]


def test_failure_alerts_once_a_day_and_retries_every_pass(env):
    with_smtp(env)
    env.days.write(MODEL_PID, "system", SUN, at(SUN, 10))
    env.engine.rc[("qt_conservative_model", MON)] = 1
    sched = env.make(portfolios=("qt_conservative_model",))
    out = dict(sched.run_pass())
    assert out["qt_conservative_model"].action == ALERT
    assert out["qt_conservative_model"].reason == "failed"
    env.clock.t = at(MON, 8, 30)
    sched.run_pass()
    assert len(env.engine.calls) == 2                          # retried
    assert len(env.sent) == 1                                  # alerted once
    to, subject, body = env.sent[0]
    assert to == ["pres@algogators.com"] and "failed" in subject and "Segmentation" in body
    log = (env.logs / cu.CATCHUP_LOG).read_text()
    assert log.count(f"ALERT {MODEL_PID} 2026-10-12 failed:") == 2   # the alert + e-mailed
    assert "e-mailed to the president" in log
    assert "engine output for qt_conservative_model 2026-10-12" in \
        (env.logs / cu.ENGINE_LOG).read_text()
    del env.engine.rc[("qt_conservative_model", MON)]
    env.clock.t = at(MON, 9)
    sched.run_pass()
    assert env.days.last_book_date(MODEL_PID, "system") == MON


def test_refusal_reason_and_distinct_reasons_alert_separately(env):
    with_smtp(env)
    env.days.write(MODEL_PID, "system", SUN, at(SUN, 10))
    env.engine.rc[("qt_conservative_model", MON)] = 2
    sched = env.make(portfolios=("qt_conservative_model",))
    sched.run_pass()
    env.engine.rc[("qt_conservative_model", MON)] = 1
    env.clock.t = at(MON, 8, 30)
    sched.run_pass()
    assert [s for _, s, _ in env.sent] == [
        f"QT alert: {MODEL_PID} 2026-10-12 refused", f"QT alert: {MODEL_PID} 2026-10-12 failed"]


def test_alerts_respect_qt_email_disabled(env):
    with_smtp(env)
    env.settings = dataclasses.replace(env.settings, email_disabled=True)
    env.days.write(MODEL_PID, "system", SUN, at(SUN, 10))
    env.engine.rc[("qt_conservative_model", MON)] = 1
    sched = CatchupScheduler(env.days, env.settings,
                             CatchupSettings(portfolios=("qt_conservative_model",)),
                             runner=env.engine, sender=lambda *a: env.sent.append(a),
                             clock=env.clock, calendar_loader=lambda: Calendar(set(), {2026}),
                             lock=free_lock)
    sched.run_pass()
    assert env.sent == []
    assert "not e-mailed (QT_EMAIL_DISABLED=1)" in (env.logs / cu.CATCHUP_LOG).read_text()


def test_exit_0_without_a_book_is_an_alert(env):
    env.days.write(MODEL_PID, "system", SUN, at(SUN, 10))
    env.engine.run = lambda argv, cwd, timeout, copy_to=None: RunResult(rc=0)
    step = env.make(portfolios=("qt_conservative_model",)).catch_up("qt_conservative_model")
    assert step.action == ALERT and step.reason == "no_book"


def test_unpublished_non_trading_day_is_an_alert(env):
    env.days.write(DESK_PID, "qt", SUN, at(SUN, 10))           # Sunday never auto-published
    step = env.make(portfolios=("qt_conservative",)).catch_up("qt_conservative")
    assert step.action == ALERT and step.reason == "not_auto_published"
    assert env.engine.calls == []


def test_calendar_gaps_and_bad_config_are_alerts(env):
    env.days.write(MODEL_PID, "system", SUN, at(SUN, 10))
    sched = CatchupScheduler(env.days, env.settings, CatchupSettings(portfolios=("nope",)),
                             runner=env.engine, clock=env.clock, lock=free_lock,
                             calendar_loader=lambda: Calendar(set(), {2025}))
    out = dict(sched.run_pass())
    assert out["nope"].reason == "config"
    step = sched.catch_up("qt_conservative_model")
    assert step.action == ALERT and step.reason == "calendar"
    assert env.engine.calls == []


def test_up_to_date_does_nothing(env):
    env.days.write(MODEL_PID, "system", MON, at(MON, 7))
    step = env.make(portfolios=("qt_conservative_model",)).catch_up("qt_conservative_model")
    assert step.action == UP_TO_DATE and env.engine.calls == []


def test_a_held_portfolio_lock_skips_the_portfolio(env):
    @contextlib.contextmanager
    def busy(path, wait):
        yield not path.endswith(f"{MODEL_PID}.lock")
    env.days.write(MODEL_PID, "system", SUN, at(SUN, 10))
    out = dict(env.make(portfolios=("qt_conservative_model",), lock=busy).run_pass())
    assert out["qt_conservative_model"].reason == "busy" and env.engine.calls == []


def test_a_held_scheduler_lock_skips_the_pass(env):
    @contextlib.contextmanager
    def held(path, wait):
        yield not path.endswith(cu.SCHEDULER_LOCK)
    assert env.make(lock=held).run_pass() is None


def test_one_failing_portfolio_does_not_stop_the_others(env):
    env.days.write(MODEL_PID, "system", SUN, at(SUN, 10))
    real = env.days.last_book_date

    def flaky(pid, book):
        if pid == DESK_PID:
            raise RuntimeError("database error (OperationalError)")
        return real(pid, book)
    env.days.last_book_date = flaky
    out = dict(env.make().run_pass())
    assert out["qt_conservative"].reason == "scheduler_error"
    assert dates_run(env) == [("qt_conservative_model", "2026-10-12")]


# -- when ---------------------------------------------------------------------------------------

def test_tick_runs_once_per_slot_inside_the_window(env):
    env.days.write(MODEL_PID, "system", MON, at(MON, 7))
    sched = env.make(portfolios=("qt_conservative_model",))
    ran = []
    sched.run_pass = lambda: ran.append(sched.local_now().strftime("%H:%M"))
    for hh, mm in ((5, 59), (6, 0), (6, 1), (6, 29), (6, 30), (12, 45), (22, 0), (22, 1),
                   (23, 30)):
        env.clock.t = at(MON, hh, mm)
        sched.tick()
    assert ran == ["06:00", "06:30", "12:45", "22:00"]


def test_tick_disabled(env):
    sched = env.make(enabled=False)
    sched.run_pass = lambda: pytest.fail("ran")
    assert sched.tick() is False


def test_tick_is_on_new_york_time_across_dst(env):
    sched = env.make()
    # 2026-11-01: EDT -> EST. 10:00 UTC is 06:00 EDT on Oct 31 and 05:00 EST on Nov 1.
    assert sched.in_window(dt.datetime(2026, 10, 31, 10, 0, tzinfo=dt.timezone.utc)
                           .astimezone(NY))
    env.clock.t = dt.datetime(2026, 11, 1, 10, 0, tzinfo=dt.timezone.utc)
    assert not sched.in_window(sched.local_now())


# -- the watchdog CLI -----------------------------------------------------------------------------

def test_watchdog_runs_a_pass_only_when_the_scheduler_is_late(env, capsys):
    env.days.write(MODEL_PID, "system", MON, at(MON, 7))
    sched = env.make(portfolios=("qt_conservative_model",))
    assert cu.main(["watchdog"], scheduler=sched) == 0
    assert "running a pass" in capsys.readouterr().out
    assert sched.heartbeat_age_s() == 0
    env.clock.t = at(MON, 8, 50)
    assert cu.main(["watchdog"], scheduler=sched) == 0
    assert "ok (last pass 50 min ago)" in capsys.readouterr().out
    env.clock.t = at(MON, 10, 0)
    cu.main(["watchdog"], scheduler=sched)
    assert "running a pass" in capsys.readouterr().out
    env.clock.t = at(MON, 23, 0)
    cu.main(["watchdog"], scheduler=sched)
    assert "outside the window" in capsys.readouterr().out
    assert cu.main(["bogus"], scheduler=sched) == 2


def test_run_once_exit_status_reports_alerts(env):
    env.days.write(MODEL_PID, "system", SUN, at(SUN, 10))
    env.engine.rc[("qt_conservative_model", MON)] = 1
    sched = env.make(portfolios=("qt_conservative_model",))
    assert cu.main(["run-once"], scheduler=sched) == 1


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
