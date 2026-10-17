"""The data-driven run (QT_EVENING_RUN=1; issues #191, #192): data-ngin pulls at 04:30 ET, so on
day T the ready row is for T-1 and today's own model run is the primary run, gated on it. Also:
builds ahead of today (the weekend chain), rebuilds on a re-pull, the wait-then-alert before
06:00, the daily data-ready check and the desk's review e-mail. The same fake clock, engine and
day store as test_catchup.py, with the run on and the recommended host settings (today's run
from 04:30). 2026-10-09 is a Friday."""

import dataclasses
import datetime as dt
import json
from pathlib import Path

import pytest

from algogators_rpc.services.desk import catchup as cu
from algogators_rpc.services.desk import review_mail
from algogators_rpc.services.desk.catchup import (ALERT, UP_TO_DATE, WAIT_PUBLISH, WAIT_TIME,
                                                  CatchupScheduler, DataReady)
from algogators_rpc.services.desk.config import (CatchupSettings, CommandSettings, ConfigError,
                                                 catchup_settings)
from algogators_rpc.services.desk.jobs import RunResult
from algogators_rpc.services.desk.trading_days import (CalendarError, calendar_from_dates,
                                                       load_calendar, parse_holidays,
                                                       resolve_holidays_path,
                                                       staleness_tolerance_days)
from test_catchup import (BINARY, DESK, DESK_PID, FRI, MODEL_PID, MON, SAT, SMTP, SUN, THU, TUE,
                          TWIN, UTC, Clock, Days, Engine, Holds, at, free_lock)

HOLIDAYS = Path(__file__).resolve().parents[3] / "include" / "trade_ngin" / "core" / \
    "holidays.json"
DATASET = "futures_ohlcv_1d"
WED = dt.date(2026, 10, 7)
REVIEWER = "dominickdupuy@ufl.edu"


class EveningDays(Days):
    """Days, plus ops.data_ready, the bars, the desk's saves and the books' positions."""

    def __init__(self):
        super().__init__()
        self.ready = []            # DataReady rows
        self.bars = {}             # symbol -> last bar date
        self.desk_edits = set()    # (pid, date) with a done save
        self.positions = {}        # (pid, book, date) -> {symbol: Line}

    def latest_data_ready(self, dataset):
        rows = [r for r in self.ready if r.dataset == dataset]
        # The table holds one row per bar date (an upsert): the newest write of it wins.
        return max(rows, key=lambda r: (r.bar_date, r.finished_at)) if rows else None

    def last_bar_dates(self, symbols, since):
        return {s: d for s, d in self.bars.items() if s in symbols and d >= since}

    def desk_edited(self, pid, day):
        return (pid, day) in self.desk_edits

    def day_books(self, pid, day):
        return {b: dict(self.positions.get((pid, b, day), {})) for b in ("system", "qt")}

    def last_published_before(self, pid, day):
        days = [d for (p, d) in self.published if p == pid and d < day]
        return max(days) if days else None


class EveningEngine(Engine):
    """The model run writes system, and qt unless the desk saved the day (QT_SEED kept)."""

    def __init__(self, days, clock):
        super().__init__(days, clock)
        self.targets = {}          # date -> {symbol: quantity} the model computes

    def run(self, argv, cwd, timeout, copy_to=None):
        d = argv[argv.index("--portfolio-config") + 1]
        day = dt.date.fromisoformat(argv[argv.index("--date") + 1])
        pid = DESK_PID if d == DESK else MODEL_PID
        model = not argv[1].startswith("--") or argv[1] == "--portfolio-config"
        kept = model and d == DESK and (pid, day) in self.days.desk_edits
        if kept:
            self.calls.append((list(argv), cwd, timeout))
            if ("model", d, day) in self.rc:
                return RunResult(rc=self.rc[("model", d, day)],
                                 stderr="FEED_HOLE_REFUSAL held symbol ES\n")
            self.days.write(pid, "system", day, self.clock())
            self.days.positions[(pid, "system", day)] = {
                s: review_mail.Line(q) for s, q in self.targets.get(day, {}).items()}
            return RunResult(rc=0, stdout="QT_SEED kept\n")
        res = super().run(argv, cwd, timeout, copy_to)
        if model and res.rc == 0:
            lines = {s: review_mail.Line(q) for s, q in self.targets.get(day, {}).items()}
            self.days.positions[(pid, "system", day)] = lines
            if d == DESK:
                self.days.positions[(pid, "qt", day)] = dict(lines)
        return res


def ready(bar_date, finished, status="complete", missing=(), loaded=36):
    return DataReady(DATASET, bar_date, 36, loaded, tuple(missing), status, "run-1",
                     finished.astimezone(UTC))


def published_day(env, day):
    """`day` is done on both portfolios: built, approved or published, and sent."""
    for pid, books in ((DESK_PID, ("system", "qt")), (MODEL_PID, ("system",))):
        for b in books:
            env.days.write(pid, b, day, at(day, 6, 45) if day >= FRI else at(day, 7))
        env.days.published[(pid, day)] = (at(day, 9), "desk@x", "desk")
        env.days.sent[(pid, day)] = at(day, 9, 30)


@pytest.fixture
def env(tmp_path):
    config = tmp_path / "config"
    for d, pid, editable in ((DESK, DESK_PID, True), (TWIN, MODEL_PID, False)):
        p = config / "portfolios" / d
        p.mkdir(parents=True)
        (p / "portfolio.json").write_text(json.dumps({"portfolio_id": pid,
                                                      "qt": {"desk_editable": editable}}))
        (p / "email.json").write_text(json.dumps(SMTP))
    (config / "defaults.json").write_text(json.dumps({"live": {
        "data_staleness_tolerance_days": 4}}))
    settings = CommandSettings(engine_binary=BINARY, engine_cwd="/app", config_dir=str(config),
                               lock_dir=str(tmp_path / "locks"), log_dir=str(tmp_path / "logs"),
                               approvers="vp=vp@algogators.com,president=pres@algogators.com")
    clock = Clock(at(FRI, 20, 30))
    days = EveningDays()
    engine = EveningEngine(days, clock)
    sent = []
    calendar = load_calendar(str(HOLIDAYS))

    def make(portfolios=(DESK, TWIN), settings_=None, **catchup):
        catchup.setdefault("today_not_before", dt.time(4, 30))   # the host .env's value
        return CatchupScheduler(days, settings_ or settings,
                                CatchupSettings(portfolios=portfolios, **catchup),
                                runner=engine, sender=lambda cfg, to, s, b: sent.append((to, s, b)),
                                clock=clock, lock=free_lock, dispatcher=Holds(),
                                calendar=calendar)

    class Env:
        pass
    e = Env()
    e.settings, e.clock, e.days, e.engine, e.sent, e.make = settings, clock, days, engine, sent, make
    e.logs = tmp_path / "logs"
    e.config = config
    return e


def calls(env):
    out = []
    for argv, _, _ in env.engine.calls:
        mode = argv[1] if argv[1] in ("--fallback", "--send") else "model"
        out.append((mode, argv[argv.index("--portfolio-config") + 1],
                    argv[argv.index("--date") + 1]))
    return out


def subjects(env, to=None):
    return [s for t, s, _ in env.sent if to is None or t == to]


def reviews(env):
    return [(s, b) for t, s, b in env.sent if t == [REVIEWER]]


def alerts(env):
    return [s for t, s, _ in env.sent if t == ["pres@algogators.com"]]


# -- the trading calendar -------------------------------------------------------------------------

def test_next_trading_day_friday_and_sunday_build_monday():
    cal = load_calendar(str(HOLIDAYS))
    assert cal.next_trading_day(FRI) == MON
    assert cal.next_trading_day(SAT) == MON
    assert cal.next_trading_day(SUN) == MON
    assert cal.next_trading_day(MON) == TUE
    assert cal.previous_trading_day(MON) == FRI
    assert cal.previous_trading_day(SUN) == FRI


@pytest.mark.parametrize("bar_date,target", [
    (dt.date(2026, 11, 25), dt.date(2026, 11, 27)),   # Thanksgiving Thursday is skipped
    (dt.date(2026, 12, 24), dt.date(2026, 12, 28)),   # Christmas Friday, then the weekend
    (dt.date(2026, 9, 4), dt.date(2026, 9, 8)),       # Labor Day Monday
    (dt.date(2026, 4, 2), dt.date(2026, 4, 6)),       # Good Friday (the engine's calendar)
])
def test_next_trading_day_skips_the_engines_holidays(bar_date, target):
    assert load_calendar(str(HOLIDAYS)).next_trading_day(bar_date) == target


def test_the_calendar_fails_closed_outside_its_years():
    cal = calendar_from_dates([], [2026])
    with pytest.raises(CalendarError):
        cal.next_trading_day(dt.date(2026, 12, 31))
    with pytest.raises(CalendarError):
        parse_holidays({"2026": [{"date": "nope"}]})
    with pytest.raises(CalendarError):
        load_calendar("/no/such/holidays.json")


def test_holidays_path_is_found_as_the_engine_finds_it(tmp_path):
    (tmp_path / "include" / "trade_ngin" / "core").mkdir(parents=True)
    path = tmp_path / "include" / "trade_ngin" / "core" / "holidays.json"
    path.write_text("{}")
    assert resolve_holidays_path(str(tmp_path), {}) == str(path)
    assert resolve_holidays_path(str(tmp_path), {"TRADE_NGIN_HOLIDAYS_JSON": "/x.json"}) == \
        "/x.json"


def test_staleness_tolerance_is_the_engines(tmp_path):
    (tmp_path / "portfolios" / "p").mkdir(parents=True)
    assert staleness_tolerance_days(str(tmp_path), "p") == 4
    (tmp_path / "defaults.json").write_text(json.dumps({"live": {
        "data_staleness_tolerance_days": 6}}))
    assert staleness_tolerance_days(str(tmp_path), "p") == 6
    (tmp_path / "portfolios" / "p" / "portfolio.json").write_text(json.dumps({"live": {
        "data_staleness_tolerance_days": 2}}))
    assert staleness_tolerance_days(str(tmp_path), "p") == 2


# -- builds ahead of today (data reaching past today: the weekend chain) ----------------------------------------------------------------------------

def test_friday_evening_builds_only_saturday_and_each_weekend_day_waits(env):
    """D-63 (decision 6): Friday's bars build Saturday; Saturday waits for the desk or its 10:00
    fallback, with the alert, like a weekday; only then is Sunday built, and Sunday waits the
    same way before Monday is built. No day is published ahead of its own day."""
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    sched = env.make(portfolios=(DESK,))
    out = dict(sched.run_pass())
    assert calls(env) == [("model", DESK, "2026-10-10")]
    assert env.days.rows == {} and env.days.published_at(DESK_PID, SAT) is None
    assert out[DESK].action == UP_TO_DATE and reviews(env) == []
    env.clock.t = at(SAT, 9, 45)
    out = dict(sched.run_pass())
    assert out[DESK].action == WAIT_PUBLISH and out[DESK].date == SAT
    env.clock.t = at(SAT, 10, 1)
    sched.run_pass()
    assert calls(env)[1:] == [("--fallback", DESK, "2026-10-10"), ("model", DESK, "2026-10-11")]
    assert [r["requested_by"] for r in env.days.rows.values()] == ["system:fallback-10am"]
    assert any(a.startswith(f"QT did not approve {DESK_PID} 2026-10-10") for a in alerts(env))
    assert env.days.published_at(DESK_PID, SUN) is None      # Sunday waits for its own day
    env.clock.t = at(SUN, 10, 1)
    sched.run_pass()
    assert calls(env)[3:] == [("--fallback", DESK, "2026-10-11"), ("model", DESK, "2026-10-12")]
    assert any(a.startswith(f"QT did not approve {DESK_PID} 2026-10-11") for a in alerts(env))
    assert all(r["requested_by"] != "system:fallback-nontrading"
               for r in env.days.rows.values())
    assert env.days.published_at(DESK_PID, MON) is None        # waits for the desk
    assert [s for s, _ in reviews(env)] == [
        f"QT review: {DESK_PID} book for 2026-10-12 (bars of 2026-10-09)"]


def test_the_twin_still_builds_ahead(env):
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    env.make(portfolios=(TWIN,)).run_pass()
    assert calls(env) == [("model", TWIN, "2026-10-10"), ("model", TWIN, "2026-10-11"),
                          ("model", TWIN, "2026-10-12")]
    assert all(env.days.published_at(MODEL_PID, d) is None for d in (SAT, SUN, MON))


def test_a_weekday_evening_builds_tomorrow(env):
    published_day(env, MON)
    env.days.ready.append(ready(MON, at(MON, 20, 5)))
    env.clock.t = at(MON, 20, 30)
    env.make().run_pass()
    assert calls(env) == [("model", DESK, "2026-10-13"), ("model", TWIN, "2026-10-13")]
    assert len(reviews(env)) == 1


def test_no_ready_row_builds_nothing(env):
    published_day(env, FRI)
    env.make().run_pass()
    assert env.engine.calls == [] and reviews(env) == []
    assert alerts(env) == [f"QT alert: {DATASET} 2026-10-08 data_not_ready"]   # the daily check


def test_old_ready_row_builds_nothing_ahead(env):
    published_day(env, FRI)
    env.days.ready.append(ready(THU, at(THU, 20, 10)))      # Friday's book came from it
    env.make().run_pass()
    assert env.engine.calls == []


def test_a_partial_pull_within_tolerance_builds(env):
    published_day(env, FRI)
    env.days.bars = {"ZN": THU}                              # Monday's run reads to Sunday: 3 days
    env.days.ready.append(ready(FRI, at(FRI, 20, 10), "partial", ("ZN",), loaded=35))
    sched = env.make(portfolios=(DESK,))
    for when in (at(FRI, 20, 30), at(SAT, 10, 1), at(SUN, 10, 1)):
        env.clock.t = when
        sched.run_pass()
    assert ("model", DESK, "2026-10-12") in calls(env)
    (_, body), = reviews(env)
    assert "ZN: last bar 2026-10-08" in body and "partial" in body


def test_a_partial_pull_past_tolerance_alerts_and_builds_nothing(env):
    published_day(env, FRI)
    env.days.bars = {"ZN": dt.date(2026, 10, 2)}             # 9 days before Sunday
    env.days.ready.append(ready(FRI, at(FRI, 20, 10), "partial", ("ZN", "GC"), loaded=34))
    out = dict(env.make(portfolios=(DESK,)).run_pass())
    assert env.engine.calls == [] and reviews(env) == []
    assert out[DESK].action == ALERT and out[DESK].reason == "data_stale"
    assert "ZN (last bar 2026-10-02)" in out[DESK].message
    assert "GC (last bar none)" in out[DESK].message
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_stale"]


def test_a_failed_pull_alerts_and_builds_nothing(env):
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10), "failed", loaded=0))
    out = dict(env.make().run_pass())
    assert env.engine.calls == []
    assert out[DESK].reason == out[TWIN].reason == "data_failed"


def test_an_engine_feed_hole_refusal_is_an_alert_and_retried(env):
    published_day(env, MON)
    env.days.ready.append(ready(MON, at(MON, 20, 5)))
    env.engine.rc[("model", DESK, TUE)] = 1
    env.engine.run_real = env.engine.run

    def refusing(argv, cwd, timeout, copy_to=None):
        if argv[1:] == ["--portfolio-config", DESK, "--date", "2026-10-13"] and \
                ("model", DESK, TUE) in env.engine.rc:
            env.engine.calls.append((list(argv), cwd, timeout))
            return RunResult(rc=1, stderr="FEED_HOLE_REFUSAL held symbol ES (quantity 2)\n")
        return env.engine.run_real(argv, cwd, timeout, copy_to)
    env.engine.run = refusing
    env.clock.t = at(MON, 20, 30)
    sched = env.make(portfolios=(DESK,))
    out = dict(sched.run_pass())
    assert out[DESK].action == ALERT and out[DESK].reason == "refused"
    env.clock.t = at(MON, 21, 0)
    sched.run_pass()
    assert len(calls(env)) == 2                              # retried after 15 minutes
    del env.engine.rc[("model", DESK, TUE)]
    env.clock.t = at(MON, 21, 30)
    sched.run_pass()
    assert env.days.last_book_date(DESK_PID, "qt") == TUE


# -- rebuilds -------------------------------------------------------------------------------------

def friday_evening(env):
    """Friday's bars at 20:10, then the weekend as D-63 runs it: Saturday and Sunday each fall
    back at 10:00, and Monday's books are built on Sunday at 10:01 (with the review e-mail)."""
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    env.engine.targets[MON] = {"ES": 2, "ZN": -1}
    sched = env.make()
    for when in (at(FRI, 20, 30), at(SAT, 10, 1), at(SUN, 10, 1)):
        env.clock.t = when
        sched.run_pass()
    return sched


def sunday_repull(env, sched):
    """Sunday's bars at 20:10, after the weekend's passes."""
    env.engine.calls.clear()
    env.days.ready.append(ready(SUN, at(SUN, 20, 10)))
    env.engine.targets[MON] = {"ES": 3, "ZN": -1}
    env.clock.t = at(SUN, 20, 30)
    sched.run_pass()


def test_a_sunday_repull_rebuilds_monday_once(env):
    sched = friday_evening(env)
    sunday_repull(env, sched)
    assert calls(env) == [("model", DESK, "2026-10-12"), ("model", TWIN, "2026-10-12")]
    assert env.days.positions[(DESK_PID, "qt", MON)]["ES"].quantity == 3
    (s1, _), (s2, b2) = reviews(env)
    assert s2 == f"QT review: {DESK_PID} book for 2026-10-12 (bars of 2026-10-11) - updated"
    assert "supersedes the one sent at" in b2
    env.engine.calls.clear()
    env.clock.t = at(SUN, 21, 0)
    sched.run_pass()                                         # the same finished_at: nothing
    assert env.engine.calls == [] and len(reviews(env)) == 2


def test_a_saturday_ready_row_builds_monday_once_sunday_is_out(env):
    """CME crypto trades on Saturdays: a Saturday bar date still builds Monday (the books keep
    the engine's calendar); Monday is built once Sunday is out, from the newest row."""
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    sched = env.make(portfolios=(DESK,))
    for when in (at(FRI, 20, 30), at(SAT, 10, 1)):
        env.clock.t = when
        sched.run_pass()
    env.days.ready.append(ready(SAT, at(SAT, 20, 5), loaded=2))
    env.clock.t = at(SAT, 20, 30)
    env.engine.calls.clear()
    sched.run_pass()
    assert env.engine.calls == []                            # Sunday is not out yet
    env.clock.t = at(SUN, 10, 1)
    sched.run_pass()
    assert ("model", DESK, "2026-10-12") in calls(env)
    assert reviews(env)[-1][0].endswith("(bars of 2026-10-10)")


def test_the_weekend_days_fall_back_at_1000_and_are_sent_then(env):
    friday_evening(env)
    assert {(p, d) for p, d in env.engine.mails} == {
        (DESK_PID, SAT), (DESK_PID, SUN), (MODEL_PID, SAT), (MODEL_PID, SUN)}
    assert env.days.published[(MODEL_PID, SAT)][2] == "model-only"
    assert env.days.published[(DESK_PID, SAT)][2] == "fallback"


def test_a_published_day_is_never_rebuilt(env):
    sched = friday_evening(env)
    env.days.published[(DESK_PID, MON)] = (at(SUN, 12), "desk@x", "desk")   # approved Sunday
    sunday_repull(env, sched)
    assert ("model", DESK, "2026-10-12") not in calls(env)
    assert len(reviews(env)) == 1


def test_desk_edits_are_kept_on_a_rebuild(env):
    sched = friday_evening(env)
    env.days.desk_edits.add((DESK_PID, MON))
    env.days.positions[(DESK_PID, "qt", MON)] = {
        "ES": review_mail.Line(1, "desk"), "ZN": review_mail.Line(-1, "margin")}
    sunday_repull(env, sched)
    assert ("model", DESK, "2026-10-12") in calls(env)
    assert env.days.positions[(DESK_PID, "system", MON)]["ES"].quantity == 3   # model rebuilt
    assert env.days.positions[(DESK_PID, "qt", MON)]["ES"].quantity == 1       # desk kept
    log = (env.logs / cu.CATCHUP_LOG).read_text()
    assert "the desk's saved edits were kept" in log
    _, body = reviews(env)[-1]
    assert "desk's saved edits for this day were kept" in body
    assert "Model book" in body and "margin" in body


def test_a_rebuild_that_writes_no_new_book_alerts(env):
    sched = friday_evening(env)
    env.days.sent[(DESK_PID, SUN)] = at(SUN, 9, 30)
    env.days.ready.append(ready(SUN, at(SUN, 20, 10)))
    env.engine.run = lambda argv, cwd, timeout, copy_to=None: RunResult(rc=0)
    env.clock.t = at(SUN, 20, 30)
    out = dict(sched.run_pass())
    assert out[DESK].reason == "no_book" and "did not rewrite" in out[DESK].message


# -- today's run, gated on the data (data-ngin's 04:30 pull) --------------------------------------

def weekend_done(env):
    """Friday, Saturday and Sunday are built, published and sent; Monday has no book."""
    for d in (FRI, SAT, SUN):
        published_day(env, d)


def test_monday_0435_flow(env):
    """Monday 04:30: the run waits quietly for the pull; Sunday's row arrives at 04:35, today's
    run goes, the desk gets one review e-mail; a re-pull rebuilds once; after the approval no
    rebuild."""
    weekend_done(env)
    env.engine.targets[MON] = {"ES": 2}
    sched = env.make()
    env.clock.t = at(MON, 4, 30)
    out = dict(sched.run_pass())
    assert env.engine.calls == [] and env.sent == []
    assert out[DESK].action == WAIT_TIME and "waits for data-ngin's" in out[DESK].message
    env.days.ready.append(ready(SUN, at(MON, 4, 35), loaded=26))
    env.clock.t = at(MON, 4, 40)
    sched.run_pass()
    assert calls(env) == [("model", DESK, "2026-10-12"), ("model", TWIN, "2026-10-12")]
    assert env.days.published[(MODEL_PID, MON)][2] == "model-only"
    assert [s for s, _ in reviews(env)] == [
        f"QT review: {DESK_PID} book for 2026-10-12 (bars of 2026-10-11)"]
    env.clock.t = at(MON, 4, 45)
    sched.run_pass()
    assert len(calls(env)) == 2 and len(reviews(env)) == 1          # once per build
    env.days.ready.append(ready(SUN, at(MON, 5, 10), loaded=26))    # a re-pull
    env.engine.targets[MON] = {"ES": 3}
    env.clock.t = at(MON, 5, 15)
    sched.run_pass()
    assert calls(env)[2:] == [("model", DESK, "2026-10-12")]        # the twin is published
    assert reviews(env)[-1][0].endswith("- updated")
    env.days.published[(DESK_PID, MON)] = (at(MON, 8), "desk@x", "desk")
    env.days.ready.append(ready(SUN, at(MON, 8, 10), loaded=26))
    env.clock.t = at(MON, 8, 15)
    sched.run_pass()
    assert len(calls(env)) == 3 and len(reviews(env)) == 2          # frozen
    assert alerts(env) == []


def test_a_weekday_run_from_the_previous_days_row(env):
    published_day(env, MON)
    env.days.ready.append(ready(MON, at(TUE, 4, 40)))
    env.clock.t = at(TUE, 4, 45)
    env.make().run_pass()
    assert calls(env) == [("model", DESK, "2026-10-13"), ("model", TWIN, "2026-10-13")]
    assert len(reviews(env)) == 1


def test_no_wait_when_the_book_of_today_exists(env):
    sched = friday_evening(env)
    env.engine.calls.clear()
    env.clock.t = at(MON, 6, 45)
    out = dict(sched.run_pass())
    assert env.engine.calls == []
    assert out[DESK].action == WAIT_PUBLISH and out[DESK].date == MON
    env.clock.t = at(MON, 10, 0)                             # the 10:00 fallback is unchanged
    sched.run_pass()
    assert calls(env)[0] == ("--fallback", DESK, "2026-10-12")
    assert (DESK_PID, MON) in env.engine.mails


def test_no_data_waits_quietly_before_0600_then_alerts(env):
    weekend_done(env)
    env.days.ready.append(ready(THU, at(FRI, 4, 40)))
    sched = env.make()
    env.clock.t = at(MON, 5, 55)
    out = dict(sched.run_pass())
    assert env.engine.calls == [] and env.sent == []
    assert out[DESK].action == WAIT_TIME
    env.clock.t = at(MON, 6, 0)
    out = dict(sched.run_pass())
    assert out[DESK].action == ALERT and out[DESK].reason == "data_not_ready"
    assert "row for 2026-10-09 at 06:00 New York (newest 2026-10-08 (complete))" in \
        out[DESK].message
    env.clock.t = at(MON, 6, 5)
    sched.run_pass()
    assert alerts(env).count(f"QT alert: {DESK_PID} 2026-10-12 data_not_ready") == 1
    assert alerts(env).count(f"QT alert: {DATASET} 2026-10-09 data_not_ready") == 1
    env.days.ready.append(ready(SUN, at(MON, 6, 7)))
    env.clock.t = at(MON, 6, 10)
    sched.run_pass()
    assert ("model", DESK, "2026-10-12") in calls(env)


@pytest.mark.parametrize("status,missing,reason", [("failed", (), "data_failed"),
                                                   ("partial", ("GC",), "data_stale")])
def test_a_final_bad_row_alerts_at_once_even_before_0600(env, status, missing, reason):
    weekend_done(env)
    env.days.ready.append(ready(SUN, at(MON, 4, 35), status, missing, loaded=20))
    env.clock.t = at(MON, 4, 40)
    out = dict(env.make(portfolios=(DESK,)).run_pass())
    assert env.engine.calls == []
    assert out[DESK].action == ALERT and out[DESK].reason == reason


# -- the daily data-ready check -------------------------------------------------------------------

def test_the_data_ready_check_alerts_once_from_0600(env):
    published_day(env, THU)
    env.days.ready.append(ready(WED, at(THU, 4, 40)))
    sched = env.make(portfolios=(TWIN,))
    env.clock.t = at(FRI, 5, 55)
    sched.run_pass()
    assert f"QT alert: {DATASET} 2026-10-08 data_not_ready" not in alerts(env)
    for hh, mm in ((6, 0), (6, 30), (21, 30)):
        env.clock.t = at(FRI, hh, mm)
        sched.run_pass()
    assert alerts(env).count(f"QT alert: {DATASET} 2026-10-08 data_not_ready") == 1


def test_the_data_ready_check_runs_on_saturday_for_fridays_data(env):
    published_day(env, FRI)
    env.days.ready.append(ready(THU, at(FRI, 4, 40)))
    env.clock.t = at(SAT, 6, 0)
    env.make(portfolios=(TWIN,)).run_pass()
    assert f"QT alert: {DATASET} 2026-10-09 data_not_ready" in alerts(env)


def test_no_data_ready_alert_when_the_data_is_in(env):
    published_day(env, FRI)
    published_day(env, SAT)
    env.days.ready.append(ready(FRI, at(SAT, 4, 40)))
    env.clock.t = at(SAT, 21, 30)
    env.make().run_pass()
    assert all("data_not_ready" not in a for a in alerts(env))


def test_a_failed_row_still_alerts_in_the_data_ready_check(env):
    published_day(env, MON)
    env.days.ready.append(ready(MON, at(TUE, 4, 35), "failed", loaded=0))
    env.clock.t = at(TUE, 6, 0)
    env.make(portfolios=(TWIN,)).run_pass()
    assert f"QT alert: {DATASET} 2026-10-12 data_not_ready" in alerts(env)


# -- the review e-mail ----------------------------------------------------------------------------

def monday_evening(env, bars=None, status="complete", missing=(), loaded=36):
    """Monday is done; Monday's bars at 20:10 build Tuesday (the clock at 20:30)."""
    published_day(env, MON)
    env.days.ready.append(ready(MON, at(MON, 20, 10), status, missing, loaded))
    env.clock.t = at(MON, 20, 30)


def test_the_review_email_is_built_once_and_says_everything(env):
    monday_evening(env, status="partial", missing=("ZN",), loaded=35)
    env.days.positions[(DESK_PID, "qt", MON)] = {"ES": review_mail.Line(1),
                                                 "CL": review_mail.Line(2)}
    env.days.bars = {"ZN": FRI}
    env.engine.targets[TUE] = {"ES": 3, "ZN": -1}
    sched = env.make(portfolios=(DESK,))
    sched.run_pass()
    env.clock.t = at(MON, 21, 0)
    sched.run_pass()                                         # a restart or a later pass
    (subject, body), = reviews(env)
    assert subject == f"QT review: {DESK_PID} book for 2026-10-13 (bars of 2026-10-12)"
    assert "trading day <b>2026-10-13</b>" in body and "bars of <b>2026-10-12</b>" in body
    assert "Held now (2026-10-12)" in body
    assert ('<td>ES</td><td align="right">1</td><td align="right">3</td>'
            '<td align="right">+2</td>') in body
    assert ('<td>CL</td><td align="right">2</td><td align="right">&ndash;</td>'
            '<td align="right">-2</td>') in body
    assert '<td>ZN</td><td align="right">&ndash;</td><td align="right">-1</td>' in body
    assert "ZN: last bar 2026-10-09" in body
    assert "https://algolens.algogators.com/" in body
    assert review_mail.DEADLINE in body
    assert "Approve before 09:30 ET or the model book is sent at 10:00." in body
    assert "supersedes" not in body and "kept" not in body
    state = json.loads((env.logs / review_mail.STATE).read_text())
    assert list(state) == [f"{DESK_PID}|2026-10-13|" + env.days.book_written_at(
        DESK_PID, TUE, "system").astimezone(UTC).isoformat()]


def test_review_email_rows_compare_the_held_book():
    built = dt.datetime(2026, 10, 10, 0, 30, tzinfo=UTC)
    subject, body = review_mail.build(review_mail.Review(
        portfolio_id=DESK_PID, target=MON, bar_date=FRI, built_at=built,
        books={"system": {"ES": review_mail.Line(3), "NQ": review_mail.Line(0)},
               "qt": {"ES": review_mail.Line(2, "margin"), "NQ": review_mail.Line(0)}},
        held_date=FRI, held={"ES": review_mail.Line(1), "CL": review_mail.Line(-2)},
        desk_edits_kept=True, data_status="complete", supersedes="2026-10-10T00:31:00+00:00",
        desk_url="https://algolens.algogators.com/"))
    assert subject.endswith("- updated")
    assert ('<tr style="background:#fff3cd"><td>ES</td><td align="right">1</td>'
            '<td align="right">2</td><td align="right">+1</td><td align="right">3</td>'
            '<td>margin</td></tr>') in body
    assert ('<td>CL</td><td align="right">-2</td><td align="right">&ndash;</td>'
            '<td align="right">+2</td>') in body
    assert "<td>NQ</td>" in body and "no symbol missing" in body
    assert "supersedes the one sent at 2026-10-10T00:31:00+00:00" in body
    assert "desk's saved edits for this day were kept" in body


def test_review_email_respects_qt_email_disabled(env):
    monday_evening(env)
    off = dataclasses.replace(env.settings, email_disabled=True)
    sched = env.make(portfolios=(DESK,), settings_=off)
    sched.run_pass()
    env.clock.t = at(MON, 21, 0)
    sched.run_pass()
    assert env.sent == []
    logged = (env.logs / review_mail.BODY_LOG).read_text()
    assert logged.count("not sent: QT_EMAIL_DISABLED=1") == 1
    assert review_mail.DEADLINE in logged
    state = json.loads((env.logs / review_mail.STATE).read_text())
    assert [v["email_disabled"] for v in state.values()] == [True]
    assert "not sent (QT_EMAIL_DISABLED=1" in (env.logs / cu.CATCHUP_LOG).read_text()


def test_review_email_goes_to_qt_review_recipients(env):
    monday_evening(env)
    env.make(portfolios=(DESK,), review_recipients=("a@x", "b@x")).run_pass()
    assert [t for t, s, _ in env.sent if s.startswith("QT review")] == [["a@x", "b@x"]]


def test_review_email_failure_alerts_and_retries(env):
    monday_evening(env)
    sched = env.make(portfolios=(DESK,))
    good = sched._sender

    def down(cfg, to, s, b):
        if s.startswith("QT review"):
            raise OSError("smtp down")
        good(cfg, to, s, b)
    sched._sender = down
    out = dict(sched.run_pass())
    assert out[DESK].reason == "review_mail_failed"
    sched._sender = good
    env.clock.t = at(MON, 21, 0)
    sched.run_pass()
    assert len(reviews(env)) == 1


def test_no_review_email_for_the_model_twin_or_after_the_cutoff(env):
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    env.make(portfolios=(TWIN,)).run_pass()
    assert reviews(env) == []


# -- settings -------------------------------------------------------------------------------------

def test_evening_settings_from_the_environment():
    s = catchup_settings({})
    assert s.evening_run is True and s.review_recipients == (REVIEWER,)
    assert s.data_alert_at == dt.time(6, 0) and s.data_ready_dataset == DATASET
    assert s.today_not_before == dt.time(6, 45)       # the host .env sets 04:30
    assert s.desk_url == "https://algolens.algogators.com/"
    s = catchup_settings({"QT_EVENING_RUN": "0", "QT_REVIEW_RECIPIENTS": "a@x, b@y",
                          "QT_DATA_READY_ALERT_AT": "22:15"})
    assert s.evening_run is False and s.review_recipients == ("a@x", "b@y")
    assert s.data_alert_at == dt.time(22, 15)
    with pytest.raises(ConfigError):
        catchup_settings({"QT_REVIEW_RECIPIENTS": "not-an-address"})


def test_evening_off_is_the_old_schedule(env):
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    env.make(evening_run=False).run_pass()
    assert env.engine.calls == [] and env.sent == []
