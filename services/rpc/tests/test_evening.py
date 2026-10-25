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
                                                  CatchupScheduler, DataReady, combine_ready)
from algogators_rpc.services.desk.config import (CatchupSettings, CommandSettings, ConfigError,
                                                 catchup_settings)
from algogators_rpc.services.desk.jobs import RunResult
from algogators_rpc.services.desk.trading_days import (CalendarError, calendar_from_dates,
                                                       load_calendar, parse_holidays,
                                                       per_contract_rolls,
                                                       resolve_holidays_path,
                                                       staleness_tolerance_days)
from test_catchup import (BINARY, DESK, DESK_PID, FRI, MODEL_PID, MON, SAT, SMTP, SUN, THU, TUE,
                          TWIN, UTC, Clock, Days, Engine, Holds, at, free_lock)

HOLIDAYS = Path(__file__).resolve().parents[3] / "include" / "trade_ngin" / "core" / \
    "holidays.json"
DATASET = "futures_ohlcv_1d"
CONTRACTS = "futures_contract_ohlcv_1d"
WED = dt.date(2026, 10, 7)
REVIEWER = "dominickdupuy@ufl.edu"


class EveningDays(Days):
    """Days, plus ops.data_ready, the bars, the desk's saves and the books' positions."""

    def __init__(self):
        super().__init__()
        self.ready = []            # DataReady rows
        self.bars = {}             # symbol -> last bar date
        self.contract_bars = {}    # root -> last contract bar date (contract_ohlcv_1d)
        self.desk_edits = set()    # (pid, date) with a done save
        self.positions = {}        # (pid, book, date) -> {symbol: Line}

    def latest_data_ready(self, dataset):
        rows = [r for r in self.ready if r.dataset == dataset]
        # The table holds one row per bar date (an upsert): the newest write of it wins.
        return max(rows, key=lambda r: (r.bar_date, r.finished_at)) if rows else None

    def last_bar_dates(self, symbols, since):
        return {s: d for s, d in self.bars.items() if s in symbols and d >= since}

    def every_symbol_last_bar(self, since):
        return {s: d for s, d in self.bars.items() if d >= since}

    def last_contract_bar_dates(self, roots, since):
        return {r: d for r, d in self.contract_bars.items() if r in roots and d >= since}

    def every_contract_root_last_bar(self, since):
        return {r: d for r, d in self.contract_bars.items() if d >= since}

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


def ready(bar_date, finished, status="complete", missing=(), loaded=36, dataset=DATASET, expected=36):
    return DataReady(dataset, bar_date, expected, loaded, tuple(missing), status, "run-1",
                     finished.astimezone(UTC))


def contract_ready(bar_date, finished, status="complete", missing=(), loaded=36):
    """The per-contract pull's row (algogators PR #78): expected = the roots, missing = roots."""
    return ready(bar_date, finished, status, missing, loaded, dataset=CONTRACTS)


def switch_on(env, d=DESK, pid=DESK_PID, editable=True):
    """portfolio.json of `d` turns per_contract_rolls on (rolls step 3)."""
    (env.config / "portfolios" / d / "portfolio.json").write_text(json.dumps(
        {"portfolio_id": pid, "qt": {"desk_editable": editable}, "per_contract_rolls": True}))


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


def reviews(env, day=None):
    """The review e-mails (of `day`'s book only, when given)."""
    return [(s, b) for t, s, b in env.sent if t == [REVIEWER]
            and (day is None or f" book for {day} " in s)]


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
    same way before Monday is built. Each day is reviewed as soon as it is built (Dominick
    2026-10-18). Unapproved, no day is published ahead of its own day."""
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    sched = env.make(portfolios=(DESK,))
    out = dict(sched.run_pass())
    assert calls(env) == [("model", DESK, "2026-10-10")]
    assert env.days.rows == {} and env.days.published_at(DESK_PID, SAT) is None
    assert out[DESK].action == UP_TO_DATE
    assert [s for s, _ in reviews(env)] == [
        f"QT review: {DESK_PID} book for 2026-10-10 (bars of 2026-10-09)"]
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
        f"QT review: {DESK_PID} book for {d} (bars of 2026-10-09)"
        for d in ("2026-10-10", "2026-10-11", "2026-10-12")]


def test_the_desk_approves_the_weekend_ahead_and_monday_is_built_on_friday_evening(env):
    """Dominick 2026-10-18: no reason to wait once the data is in. Friday's bars build Saturday;
    the desk approves it at once, which builds Sunday; approving Sunday builds Monday. Each day
    is still sent at 09:30 on its own day."""
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    sched = env.make(portfolios=(DESK,))
    sched.run_pass()
    assert calls(env) == [("model", DESK, "2026-10-10")]
    for day, nxt in ((SAT, "2026-10-11"), (SUN, "2026-10-12")):
        env.clock.t += dt.timedelta(minutes=10)
        env.days.published[(DESK_PID, day)] = (env.clock(), "desk@x", "desk")   # approved now
        sched.run_pass()
        assert calls(env)[-1] == ("model", DESK, nxt)
    assert [s.split(" book for ")[1][:10] for s, _ in reviews(env)] == [
        "2026-10-10", "2026-10-11", "2026-10-12"]
    assert env.engine.mails == [] and env.days.rows == {}     # nothing sent ahead of its day
    env.clock.t = at(SAT, 9, 30)
    sched.run_pass()
    assert ("--send", DESK, "2026-10-10") in calls(env)
    assert ("--send", DESK, "2026-10-11") not in calls(env)


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
    (_, body), = reviews(env, MON)
    assert "ZN: last bar 2026-10-08" in body and "partial" in body


def test_a_partial_pull_past_tolerance_alerts_and_builds_nothing(env):
    """Ruling 21: three missing symbols past the tolerance, more than N = 2: held."""
    published_day(env, FRI)
    env.days.bars = {"ZN": dt.date(2026, 10, 2), "CL": dt.date(2026, 10, 6)}   # 9 and 5 days before Sunday
    env.days.ready.append(ready(FRI, at(FRI, 20, 10), "partial", ("ZN", "GC", "CL"), loaded=33))
    out = dict(env.make(portfolios=(DESK,)).run_pass())
    assert env.engine.calls == [] and reviews(env) == []
    assert out[DESK].action == ALERT and out[DESK].reason == "data_stale"
    assert ("GC (last bar none), ZN (last bar 2026-10-02), CL (last bar 2026-10-06). The books of "
            "2026-10-12 are not built") in out[DESK].message
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_stale"]


def test_a_failed_pull_with_nothing_in_the_table_alerts_and_builds_nothing(env):
    """A failed continuous pull counts every symbol missing; none of the 36 has a bar in the table, so
    all count as stale: held (before ruling 21 every failed pull held, data_failed)."""
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10), "failed", loaded=0))
    out = dict(env.make().run_pass())
    assert env.engine.calls == []
    assert out[DESK].reason == out[TWIN].reason == "data_stale"
    assert "36 symbol(s) not named, with no bar in the table" in out[DESK].message


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
    (s1, _), (s2, b2) = reviews(env, MON)
    assert s2 == f"QT review: {DESK_PID} book for 2026-10-12 (bars of 2026-10-11) - updated"
    assert "supersedes the one sent at" in b2
    env.engine.calls.clear()
    env.clock.t = at(SUN, 21, 0)
    sched.run_pass()                                         # the same finished_at: nothing
    assert env.engine.calls == [] and len(reviews(env, MON)) == 2


def test_a_saturday_ready_row_builds_monday_once_sunday_is_out(env):
    """CME crypto trades on Saturdays: a Saturday bar date still builds Monday (the books keep
    the engine's calendar); Monday is built once Sunday is out, from the newest row."""
    published_day(env, FRI)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    sched = env.make(portfolios=(DESK,))
    for when in (at(FRI, 20, 30), at(SAT, 10, 1)):
        env.clock.t = when
        sched.run_pass()
    env.days.ready.append(ready(SAT, at(SAT, 20, 5), loaded=2, expected=2))
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
    assert len(reviews(env, MON)) == 1


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
    env.days.ready.append(ready(SUN, at(MON, 4, 35), loaded=26, expected=26))
    env.clock.t = at(MON, 4, 40)
    sched.run_pass()
    assert calls(env) == [("model", DESK, "2026-10-12"), ("model", TWIN, "2026-10-12")]
    assert env.days.published[(MODEL_PID, MON)][2] == "model-only"
    assert [s for s, _ in reviews(env)] == [
        f"QT review: {DESK_PID} book for 2026-10-12 (bars of 2026-10-11)"]
    env.clock.t = at(MON, 4, 45)
    sched.run_pass()
    assert len(calls(env)) == 2 and len(reviews(env)) == 1          # once per build
    env.days.ready.append(ready(SUN, at(MON, 5, 10), loaded=26, expected=26))    # a re-pull
    env.engine.targets[MON] = {"ES": 3}
    env.clock.t = at(MON, 5, 15)
    sched.run_pass()
    assert calls(env)[2:] == [("model", DESK, "2026-10-12")]        # the twin is published
    assert reviews(env)[-1][0].endswith("- updated")
    env.days.published[(DESK_PID, MON)] = (at(MON, 8), "desk@x", "desk")
    env.days.ready.append(ready(SUN, at(MON, 8, 10), loaded=26, expected=26))
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


@pytest.mark.parametrize("status,missing,reason", [("failed", (), "data_stale"),
                                                   ("partial", ("GC", "ZN", "CL"), "data_stale"),
                                                   ("held", (), "data_failed")])
def test_a_final_bad_row_alerts_at_once_even_before_0600(env, status, missing, reason):
    weekend_done(env)
    env.days.ready.append(ready(SUN, at(MON, 4, 35), status, missing, loaded=20 - len(missing),
                                expected=20))
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
    assert s.data_alert_at == dt.time(6, 0) and s.data_ready_datasets == (DATASET,)
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


# -- the Sunday reminder and the VP and President's log (Dominick 2026-10-18) ----------------------

def test_sunday_evening_reminds_the_desk_vp_and_president_once_while_monday_is_not_approved(env):
    for day in (FRI, SAT, SUN):
        published_day(env, day)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    sched = env.make(portfolios=(DESK,))
    env.clock.t = at(SUN, 17, 55)
    sched.run_pass()
    assert not [s for _, s, _ in env.sent if s.startswith("QT reminder")]
    for when in (at(SUN, 18, 5), at(SUN, 19, 5)):
        env.clock.t = when
        sched.run_pass()
    reminders = [(t, s, b) for t, s, b in env.sent if s.startswith("QT reminder")]
    assert len(reminders) == 1
    to, subject, body = reminders[0]
    assert to == ["pres@algogators.com", REVIEWER, "vp@algogators.com"]
    assert subject == f"QT reminder: the {DESK_PID} book for 2026-10-12 is not approved yet"
    assert "is built and waits for the desk&#x27;s approval" in body


def test_no_sunday_reminder_once_monday_is_approved(env):
    for day in (FRI, SAT, SUN):
        published_day(env, day)
    env.days.ready.append(ready(FRI, at(FRI, 20, 10)))
    env.days.write(DESK_PID, "system", MON, at(SAT, 12))
    env.days.published[(DESK_PID, MON)] = (at(SAT, 12), "desk@x", "desk")
    env.clock.t = at(SUN, 18, 5)
    env.make(portfolios=(DESK,)).run_pass()
    assert not [s for _, s, _ in env.sent if s.startswith("QT reminder")]


def _command(id_, kind, by, status="done", payload=None, minute=0, message="ok"):
    from algogators_rpc.services.desk.command_store import CommandRow
    return CommandRow(id=id_, portfolio_id=DESK_PID, date=MON, kind=kind, status=status,
                      requested_by=by, reason="r", payload=payload or {}, message=message,
                      finished_at=at(SAT, 11, minute).astimezone(UTC))


def test_every_desk_action_and_settings_change_e_mails_the_vp_and_president_once(env):
    from algogators_rpc.services.desk.activity_mail import SettingsVersion
    published_day(env, FRI)
    env.clock.t = at(SAT, 11, 0)
    sched = env.make(portfolios=(DESK,))
    env.days.commands = [_command(1, "save", "desk@x")]   # before the first start: not e-mailed
    sched.run_pass()
    log = lambda: [(t, s) for t, s, _ in env.sent if s.startswith("QT log")]  # noqa: E731
    assert log() == []
    env.days.commands += [
        _command(2, "publish", "desk@x", minute=5),
        _command(3, "publish", "system:fallback-10am", minute=6),   # the scheduler's own
        _command(4, "override_decision", "vp@algogators.com", payload={"approved": False},
                 minute=7),
        _command(5, "rescind", "trader@x", status="refused", minute=8, message="sent already"),
    ]
    env.days.settings = [SettingsVersion(9, DESK_PID, 4, "lower R", "desk@x",
                                         at(SAT, 11, 9).astimezone(UTC),
                                         {"risk": {"max_drawdown": 0.25}},
                                         {"risk": {"max_drawdown": 0.3}})]
    env.clock.t = at(SAT, 11, 15)
    sched.run_pass()
    to = ["vp@algogators.com", "pres@algogators.com"]
    assert log() == [
        (to, f"QT log: desk@x approved {DESK_PID} 2026-10-12 (done)"),
        (to, f"QT log: vp@algogators.com rejected the override of {DESK_PID} 2026-10-12 (done)"),
        (to, f"QT log: trader@x rescinded the approval of {DESK_PID} 2026-10-12 (refused)"),
        (to, f"QT log: desk@x changed the {DESK_PID} settings (version 4)"),
    ]
    body = [b for _, s, b in env.sent if "settings (version 4)" in s][0]
    assert "risk.max_drawdown: 0.3 -&gt; 0.25" in body and "lower R" in body
    env.clock.t = at(SAT, 11, 30)
    sched.run_pass()
    assert len(log()) == 4                                  # nothing twice
    # A row finished under two minutes ago waits a pass (its transaction may still commit
    # behind a later one).
    env.days.commands.append(_command(6, "save", "desk@x", minute=29))
    sched.run_pass()
    assert len(log()) == 4
    env.clock.t = at(SAT, 11, 35)
    sched.run_pass()
    assert log()[-1][1] == f"QT log: desk@x saved edits to {DESK_PID} 2026-10-12 (done)"


# -- the per-contract data (rolls step 4, ROLLS_STEP1_PLAN section 5a) ------------------------------

def test_the_datasets_are_a_list():
    s = catchup_settings({"QT_DATA_READY_DATASET": f" {DATASET}, {CONTRACTS},{DATASET} "})
    assert s.data_ready_datasets == (DATASET, CONTRACTS)
    assert catchup_settings({"QT_DATA_READY_DATASET": " , "}).data_ready_datasets == (DATASET,)
    with pytest.raises(ConfigError):
        catchup_settings({"QT_DATA_READY_DATASET": "futures_ohlcv_1d;drop"})


def test_per_contract_rolls_is_read_as_the_engine_reads_it(tmp_path):
    (tmp_path / "portfolios" / "p").mkdir(parents=True)
    assert per_contract_rolls(str(tmp_path), "p") is False                    # absent: off
    (tmp_path / "defaults.json").write_text(json.dumps({"per_contract_rolls": True}))
    assert per_contract_rolls(str(tmp_path), "p") is True
    pj = tmp_path / "portfolios" / "p" / "portfolio.json"
    pj.write_text(json.dumps({"per_contract_rolls": False}))                  # the portfolio's wins
    assert per_contract_rolls(str(tmp_path), "p") is False
    pj.write_text(json.dumps({"per_contract_rolls": "yes"}))                  # the engine refuses it
    assert per_contract_rolls(str(tmp_path), "p") is True


def test_combine_ready_is_the_oldest_date_every_dataset_has():
    a = ready(SUN, at(MON, 4, 35), "partial", ("ZN",), loaded=35)
    b = contract_ready(SAT, at(MON, 4, 50), "complete")
    c = combine_ready([a, b])
    assert c.bar_date == SAT and c.parts == (b,) and c.status == "complete"   # a is newer: in
    assert c.finished_at == b.finished_at and c.missing == ()
    d = contract_ready(SUN, at(MON, 4, 40), "partial", ("GC", "ZN"), loaded=34)
    c = combine_ready([a, d])
    assert c.bar_date == SUN and c.status == "partial" and c.parts == (a, d)
    assert c.missing == ("ZN", "GC") and c.loaded == 69 and c.expected == 72
    assert c.finished_at == d.finished_at                                     # the later of the two
    assert combine_ready([d, contract_ready(SUN, at(MON, 4, 1), "failed")]).status == "failed"
    assert combine_ready([a]) is a


def test_switch_on_monday_waits_for_the_per_contract_row_with_the_same_alert(env):
    """A late per-contract row delays the run like a late continuous row: a quiet wait before
    06:00, then the run's alert and the daily check's, named for the per-contract dataset. The
    switch-off twin never waits for it."""
    weekend_done(env)
    switch_on(env)
    sched = env.make()
    env.days.ready.append(ready(SUN, at(MON, 4, 35), loaded=26, expected=26))
    env.clock.t = at(MON, 4, 40)
    out = dict(sched.run_pass())
    assert calls(env) == [("model", TWIN, "2026-10-12")]
    assert out[DESK].action == WAIT_TIME
    assert out[DESK].message == f"today's run waits for data-ngin's {CONTRACTS} row for 2026-10-09"
    env.clock.t = at(MON, 6, 0)
    out = dict(sched.run_pass())
    assert out[DESK].action == ALERT and out[DESK].reason == "data_not_ready"
    assert f"has no {CONTRACTS} row for 2026-10-09 at 06:00 New York (no row at all)" in \
        out[DESK].message
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_not_ready",
                           f"QT alert: {CONTRACTS} 2026-10-09 data_not_ready"]
    env.days.ready.append(contract_ready(THU, at(MON, 6, 3)))   # an older bar date: still late
    env.clock.t = at(MON, 6, 5)
    out = dict(sched.run_pass())
    assert out[DESK].action == ALERT and "(newest 2026-10-08 (complete))" in out[DESK].message
    assert len(calls(env)) == 1
    env.days.ready.append(contract_ready(SUN, at(MON, 6, 7)))
    env.clock.t = at(MON, 6, 10)
    sched.run_pass()
    assert calls(env)[1:] == [("model", DESK, "2026-10-12")]
    assert len(alerts(env)) == 2                                 # once a day each


def test_two_late_datasets_are_both_named(env):
    weekend_done(env)
    switch_on(env)
    env.days.ready.append(ready(THU, at(FRI, 4, 40)))
    env.clock.t = at(MON, 6, 0)
    out = dict(env.make(portfolios=(DESK,)).run_pass())
    assert out[DESK].reason == "data_not_ready"
    assert (f"no {DATASET}, {CONTRACTS} row for 2026-10-09 at 06:00 New York ({DATASET}: newest "
            f"2026-10-08 (complete); {CONTRACTS}: no row at all)") in out[DESK].message
    assert sorted(alerts(env)) == sorted([f"QT alert: {DESK_PID} 2026-10-12 data_not_ready",
                                          f"QT alert: {DATASET} 2026-10-09 data_not_ready",
                                          f"QT alert: {CONTRACTS} 2026-10-09 data_not_ready"])


def test_switch_off_never_reads_the_per_contract_row(env):
    weekend_done(env)
    env.days.ready.append(ready(SUN, at(MON, 4, 35), loaded=26, expected=26))
    env.days.ready.append(contract_ready(SUN, at(MON, 4, 36), "failed", loaded=0))
    env.clock.t = at(MON, 6, 30)
    env.make().run_pass()
    assert calls(env) == [("model", DESK, "2026-10-12"), ("model", TWIN, "2026-10-12")]
    assert alerts(env) == []


def test_a_late_per_contract_row_holds_the_evening_build_and_its_rebuild(env):
    switch_on(env)
    published_day(env, MON)
    env.days.ready.append(ready(MON, at(MON, 20, 5)))
    env.clock.t = at(MON, 20, 30)
    sched = env.make(portfolios=(DESK,))
    sched.run_pass()
    assert calls(env) == []                                      # Monday's contract bars not in
    env.days.ready.append(contract_ready(MON, at(MON, 20, 40)))
    env.clock.t = at(MON, 20, 45)
    sched.run_pass()
    assert calls(env) == [("model", DESK, "2026-10-13")]
    env.clock.t = at(MON, 21, 0)
    sched.run_pass()
    assert len(calls(env)) == 1                                  # the same finished_at: nothing
    env.days.ready.append(contract_ready(MON, at(MON, 21, 10)))    # a re-pull of the contract bars
    env.clock.t = at(MON, 21, 15)
    sched.run_pass()
    assert calls(env)[1:] == [("model", DESK, "2026-10-13")]
    assert reviews(env)[-1][0].endswith("- updated")


def test_a_partial_per_contract_row_is_judged_on_the_contract_bars(env):
    """One root stale for many days runs, carried, with its alert (ruling 21); with N = 0 it holds."""
    switch_on(env)
    weekend_done(env)
    env.days.ready.append(ready(SUN, at(MON, 4, 35), loaded=26, expected=26))
    env.days.ready.append(contract_ready(SUN, at(MON, 4, 36), "partial", ("GC",), loaded=35))
    env.days.bars = {"GC.v.0": SUN}                              # the continuous bars are no help
    env.days.contract_bars = {"GC": dt.date(2026, 10, 2)}        # 9 days before Sunday
    env.clock.t = at(MON, 4, 40)
    out = dict(env.make(portfolios=(DESK,), contract_data_ready_max_missing=0).run_pass())
    assert out[DESK].action == ALERT and out[DESK].reason == "data_stale"
    assert out[DESK].message.startswith(f"{CONTRACTS} 2026-10-11 is partial (35 of 36); 1 root(s) have had no "
                                        "contract bar for more than 4 day(s) as of 2026-10-11")
    assert "GC (last bar 2026-10-02)" in out[DESK].message and calls(env) == []
    env.days.ready.append(contract_ready(SUN, at(MON, 4, 50), "partial", ("GC",), loaded=35))
    env.clock.t = at(MON, 4, 55)
    env.make(portfolios=(DESK,)).run_pass()                      # N = 2: one stale root runs
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert alerts(env)[-1] == f"QT alert: {DESK_PID} 2026-10-12 contract_data_partial"
    (_, body), = reviews(env, MON)
    assert "GC: last bar 2026-10-02" in body


# -- the per-contract row's rule (HD 2026-10-24, plan 11c ruling 21: an AND condition) ----------------
# Held only when more than N roots (QT_CONTRACT_DATA_READY_MAX_MISSING, 2) have EACH had no contract bar
# for more than X days (QT_CONTRACT_DATA_READY_STALE_DAYS, unset: live.data_staleness_tolerance_days, 4);
# otherwise the run goes and every missing root is alerted.

ROOTS36 = ("6A", "6B", "6C", "6E", "6J", "6L", "6M", "6N", "6S", "CL", "HO", "NG", "RB", "GC", "SI", "HG",
           "PL", "ZC", "ZS", "ZW", "KE", "ZL", "ZM", "ZR", "LE", "HE", "GF", "ZT", "ZF", "ZN", "UB", "MES",
           "MNQ", "MYM", "M2K", "MBT")


def _monday_contract(env, status, missing, loaded, bars=None, **catchup):
    switch_on(env)
    weekend_done(env)
    env.days.ready.append(ready(SUN, at(MON, 4, 35), loaded=26, expected=26))
    env.days.ready.append(contract_ready(SUN, at(MON, 4, 36), status, missing, loaded=loaded))
    env.days.contract_bars = bars if bars is not None else {r: SUN for r in ROOTS36}
    for r in missing:
        env.days.contract_bars.setdefault(r, FRI)
        if r in env.days.contract_bars and env.days.contract_bars[r] >= SUN:
            env.days.contract_bars[r] = FRI                    # a missing root has no bar of the row's date
    env.clock.t = at(MON, 4, 40)
    return dict(env.make(portfolios=(DESK,), **catchup).run_pass())


def test_one_or_two_missing_roots_run_with_an_alert(env):
    out = _monday_contract(env, "partial", ("GC", "ZN"), 34)
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 contract_data_partial"]


def test_one_or_two_unnamed_missing_roots_raise_the_partial_alert(env):
    """R4A audit A, Q7: a row that misses roots without naming them (34 of 36, no names) still alerts:
    the roots are the ones whose newest contract bar is before the row's date (GC and ZN, Friday's)."""
    bars = {r: SUN for r in ROOTS36}
    bars.update(GC=FRI, ZN=FRI)
    out = _monday_contract(env, "partial", (), 34, bars=bars)
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 contract_data_partial"]
    (_, message), = [(t, m) for t, s, m in env.sent if t == ["pres@algogators.com"]]
    assert "GC (last bar 2026-10-09)" in message and "ZN (last bar 2026-10-09)" in message


def test_three_missing_roots_within_the_days_run_with_an_alert(env):
    """More than N roots missing, each with a bar within X days: the run goes (the count alone held it
    before ruling 21)."""
    out = _monday_contract(env, "partial", ("GC", "ZN", "CL"), 33)
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 contract_data_partial"]


def test_a_one_day_zero_of_36_per_contract_row_runs(env):
    """No names, 0 of 36 loaded, every root's newest bar Friday's (two days before the run's last bar date,
    within X = 4): the run goes, every root carried, alerted."""
    out = _monday_contract(env, "partial", (), 0, bars={r: FRI for r in ROOTS36})
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 contract_data_partial"]


def test_a_one_day_failed_per_contract_pull_runs(env):
    """Ruling 21: a one-day failed pull runs with everything carried; failed for longer than X days, held."""
    out = _monday_contract(env, "failed", (), 0, bars={r: FRI for r in ROOTS36})
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT


def test_a_failed_per_contract_pull_past_the_days_alerts_and_builds_nothing(env):
    out = _monday_contract(env, "failed", (), 0, bars={r: dt.date(2026, 10, 2) for r in ROOTS36})
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert out[DESK].message.startswith(f"{CONTRACTS} 2026-10-11 is failed (0 of 36); 36 root(s) have had no "
                                        "contract bar for more than 4 day(s)")


def test_a_failed_per_contract_pull_with_no_bars_in_the_table_holds(env):
    """No contract bar of any root in the lookback: every missing root counts as stale."""
    out = _monday_contract(env, "failed", (), 0, bars={})
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert "36 root(s) not named, with no contract bar in the table" in out[DESK].message


def test_three_roots_stale_past_the_days_hold_the_run(env):
    """N + 1 roots each without a contract bar for more than X days: held, the alert naming them."""
    bars = {r: SUN for r in ROOTS36}
    bars.update(GC=dt.date(2026, 10, 2), ZN=dt.date(2026, 10, 1), CL=dt.date(2026, 10, 6))
    out = _monday_contract(env, "partial", ("GC", "ZN", "CL"), 33, bars=bars)
    assert env.engine.calls == []
    assert out[DESK].action == ALERT and out[DESK].reason == "data_stale"
    assert ("3 root(s) have had no contract bar for more than 4 day(s) as of 2026-10-11, more than "
            "QT_CONTRACT_DATA_READY_MAX_MISSING=2: ZN (last bar 2026-10-01), GC (last bar 2026-10-02), "
            "CL (last bar 2026-10-06)") in out[DESK].message          # Q1: the stalest first


def test_two_roots_stale_past_the_days_and_one_fresh_run(env):
    bars = {r: SUN for r in ROOTS36}
    bars.update(GC=dt.date(2026, 10, 2), ZN=dt.date(2026, 10, 1), CL=FRI)
    out = _monday_contract(env, "partial", ("GC", "ZN", "CL"), 33, bars=bars)
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 contract_data_partial"]


def test_the_two_settings_move_the_rule(env):
    bars = {r: SUN for r in ROOTS36}
    bars.update(GC=dt.date(2026, 10, 2), ZN=dt.date(2026, 10, 1), CL=dt.date(2026, 10, 6))
    out = _monday_contract(env, "partial", ("GC", "ZN", "CL"), 33, bars=dict(bars),
                           contract_data_ready_max_missing=3)
    assert calls(env) == [("model", DESK, "2026-10-12")]          # N = 3: three stale roots run
    s = catchup_settings({"QT_CONTRACT_DATA_READY_MAX_MISSING": "5"})
    assert s.contract_data_ready_max_missing == 5
    assert catchup_settings({}).contract_data_ready_max_missing == 2
    assert catchup_settings({}).contract_data_ready_stale_days is None
    assert catchup_settings({"QT_CONTRACT_DATA_READY_STALE_DAYS": "7"}).contract_data_ready_stale_days == 7
    with pytest.raises(ConfigError):
        catchup_settings({"QT_CONTRACT_DATA_READY_MAX_MISSING": "-1"})
    with pytest.raises(ConfigError):
        catchup_settings({"QT_CONTRACT_DATA_READY_STALE_DAYS": "four"})


def test_the_stale_days_setting_is_x(env):
    """X = 1: three roots with Friday's bar (two days old) are stale past X, more than N: held."""
    out = _monday_contract(env, "partial", ("GC", "ZN", "CL"), 33, contract_data_ready_stale_days=1)
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert "for more than 1 day(s)" in out[DESK].message


def test_a_partial_continuous_row_within_the_tolerance_runs_with_the_alert(env):
    """Switch off: a "partial, 33 of 36" continuous row naming three symbols, each last bar within the
    tolerance, runs (Q1: now with the data_partial alert naming them)."""
    weekend_done(env)
    names = ("ES", "ZN", "GC")
    env.days.bars = {s: FRI for s in names}
    env.days.ready.append(ready(SUN, at(MON, 4, 35), "partial", names, loaded=33))
    env.clock.t = at(MON, 4, 40)
    env.make(portfolios=(DESK,)).run_pass()
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_partial"]
    (_, message), = [(t, m) for t, s, m in env.sent if t == ["pres@algogators.com"]]
    assert "ES (last bar 2026-10-09)" in message and "ZN (last bar 2026-10-09)" in message


# -- R4B fix round 1 (audit A S1, S2, p01) --------------------------------------------------------------

def _monday_failed_zero_expected(env, bars):
    """A failed per-contract row that records expected = 0 (migration 028 allows it)."""
    switch_on(env)
    weekend_done(env)
    env.days.ready.append(ready(SUN, at(MON, 4, 35), loaded=26, expected=26))
    env.days.ready.append(DataReady(CONTRACTS, SUN, 0, 0, (), "failed", "run-1", at(MON, 4, 36).astimezone(UTC)))
    env.days.contract_bars = bars
    env.clock.t = at(MON, 4, 40)
    return dict(env.make(portfolios=(DESK,)).run_pass())


def test_a_failed_row_with_expected_zero_counts_every_root_the_table_knows(env):
    """S1: expected = 0 on a failed row no longer runs in silence: every root the table knows is missing;
    nine days old, more than N past X: held."""
    out = _monday_failed_zero_expected(env, {r: dt.date(2026, 10, 2) for r in ROOTS36})
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert "36 root(s) have had no contract bar for more than 4 day(s)" in out[DESK].message


def test_a_failed_row_with_expected_zero_and_fresh_bars_runs_with_the_alert(env):
    out = _monday_failed_zero_expected(env, {r: FRI for r in ROOTS36})
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 contract_data_partial"]


def test_a_failed_row_with_nothing_known_holds(env):
    out = _monday_failed_zero_expected(env, {})
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert "no root can be judged" in out[DESK].message


def test_unnamed_missing_roots_are_taken_stalest_first(env):
    """S2: 33 of 36, no names; ZC, ZN and ZS nine days stale, 6A and 6B with Friday's bar: the three
    unnamed roots are ZC, ZN and ZS (not 6A, 6B, ZC alphabetically), more than N past X: held."""
    bars = {r: SUN for r in ROOTS36}
    bars.update({"6A": FRI, "6B": FRI, "ZC": dt.date(2026, 10, 2), "ZN": dt.date(2026, 10, 2), "ZS": dt.date(2026, 10, 2)})
    out = _monday_contract(env, "partial", (), 33, bars=bars)
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert "ZC (last bar 2026-10-02), ZN (last bar 2026-10-02), ZS (last bar 2026-10-02)" in out[DESK].message
    assert "6A" not in out[DESK].message


def test_x_days_old_is_not_stale_and_one_more_is(env):
    """p01, ruling 21's "more than X days": three roots last seen exactly 4 days before the run's last bar
    date (10-07 against 10-11) run; 5 days (10-06) hold."""
    bars = {r: SUN for r in ROOTS36}
    bars.update({r: dt.date(2026, 10, 7) for r in ("GC", "ZN", "CL")})
    _monday_contract(env, "partial", ("GC", "ZN", "CL"), 33, bars=bars)
    assert calls(env) == [("model", DESK, "2026-10-12")]


def test_one_more_day_than_x_is_stale(env):
    bars = {r: SUN for r in ROOTS36}
    bars.update({r: dt.date(2026, 10, 6) for r in ("GC", "ZN", "CL")})
    out = _monday_contract(env, "partial", ("GC", "ZN", "CL"), 33, bars=bars)
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"


# -- the continuous row's rule (Q1; HD 2026-10-24, plan 11c ruling 21: the same AND condition) ---------
# Held only when more than N symbols (QT_DATA_READY_MAX_MISSING, 2) have EACH had no bar for more than X
# days (the portfolio's live.data_staleness_tolerance_days, 4) as of the run's last bar date (Sunday
# 2026-10-11 for Monday's run); otherwise the run goes and every missing symbol is alerted (data_partial).
# Before Q1 one missing symbol past X held the run, and a failed pull always held (data_failed).

def _monday_continuous(env, status, missing, loaded, bars=None, expected=36, **catchup):
    """Switch off: Sunday's continuous row arrives at 04:35, Monday's run is looked at 04:40."""
    weekend_done(env)
    env.days.ready.append(ready(SUN, at(MON, 4, 35), status, missing, loaded, expected=expected))
    env.days.bars = dict(bars) if bars is not None else {s: SUN for s in ROOTS36}
    env.clock.t = at(MON, 4, 40)
    return dict(env.make(portfolios=(DESK,), **catchup).run_pass())


def _stale(n, fresh=()):
    """The 36 symbols with Sunday's bar, but the first `n` of STALE nine days old and `fresh` Friday's."""
    bars = {s: SUN for s in ROOTS36}
    bars.update({s: dt.date(2026, 10, 2) for s in STALE[:n]})
    bars.update({s: FRI for s in fresh})
    return bars


STALE = ("GC", "ZN", "CL")


def president_messages(env):
    return [m for t, s, m in env.sent if t == ["pres@algogators.com"]]


@pytest.mark.parametrize("n", [0, 1, 2])
def test_up_to_n_symbols_past_x_run_with_the_alert(env, n):
    """0, 1 and N = 2 missing symbols past X (with ES missing but fresh): the run goes, every missing
    symbol named in the data_partial alert. Before Q1, 1 and 2 held."""
    missing = ("ES",) + STALE[:n]
    out = _monday_continuous(env, "partial", missing, 36 - len(missing), bars=_stale(n, fresh=("ES",)))
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_partial"]
    (message,) = president_messages(env)
    for s in missing:
        assert f"{s} (last bar " in message
    assert "The books of 2026-10-12 are built" in message


def test_n_plus_one_symbols_past_x_hold_the_run(env):
    out = _monday_continuous(env, "partial", ("ES",) + STALE, 32, bars=_stale(3, fresh=("ES",)))
    assert env.engine.calls == [] and reviews(env) == []
    assert out[DESK].action == ALERT and out[DESK].reason == "data_stale"
    assert out[DESK].message.startswith(
        f"{DATASET} 2026-10-11 is partial (32 of 36); 3 symbol(s) have had no bar for more than 4 day(s) as "
        "of 2026-10-11, more than QT_DATA_READY_MAX_MISSING=2: ")
    assert "ES" not in out[DESK].message.split(": ", 1)[1]           # ES is fresh: not named as stale
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_stale"]


def test_n_plus_one_missing_symbols_none_past_x_run(env):
    """Three symbols missing, each last printed Friday (two days before Sunday, within X): the run goes."""
    out = _monday_continuous(env, "partial", STALE, 33, bars=_stale(0, fresh=STALE))
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_partial"]


def test_a_one_day_failed_continuous_pull_runs_with_everything_carried(env):
    """Ruling 21: a failed pull counts every symbol the table knows as missing; each last printed
    Friday, none past X: the run goes, the alert naming all 36. Before Q1: held (data_failed)."""
    out = _monday_continuous(env, "failed", (), 0, bars={s: FRI for s in ROOTS36})
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_partial"]
    (message,) = president_messages(env)
    assert message.count("(last bar 2026-10-09)") == 36


def test_a_failed_continuous_pull_with_n_plus_one_past_x_holds(env):
    out = _monday_continuous(env, "failed", (), 0, bars=_stale(3, fresh=ROOTS36[:9]))
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert out[DESK].message.startswith(f"{DATASET} 2026-10-11 is failed (0 of 36); 3 symbol(s) have had no "
                                        "bar for more than 4 day(s)")


def test_a_failed_continuous_pull_with_n_past_x_runs(env):
    _monday_continuous(env, "failed", (), 0, bars=_stale(2))
    assert calls(env) == [("model", DESK, "2026-10-12")]


def test_a_failed_continuous_row_with_expected_zero_counts_every_symbol_the_table_knows(env):
    """A failed row recording expected = 0: the symbols are the table's, then the rule."""
    out = _monday_continuous(env, "failed", (), 0, bars={s: FRI for s in ROOTS36}, expected=0)
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_partial"]
    assert president_messages(env)[0].count("(last bar 2026-10-09)") == 36


def test_a_failed_continuous_row_with_expected_zero_and_stale_bars_holds(env):
    out = _monday_continuous(env, "failed", (), 0, bars=_stale(3), expected=0)
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert "3 symbol(s) have had no bar for more than 4 day(s)" in out[DESK].message


def test_a_failed_continuous_row_with_expected_zero_and_nothing_known_holds(env):
    out = _monday_continuous(env, "failed", (), 0, bars={}, expected=0)
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert "no symbol can be judged" in out[DESK].message


def test_exactly_x_days_is_not_stale(env):
    """GC and ZN last seen 5 days before Sunday (10-06, past X), CL exactly 4 (10-07, not past X): two
    past X, the run goes; with CL one day older (next test) three are, held. Before Q1 GC alone held."""
    bars = {s: SUN for s in ROOTS36}
    bars.update(GC=dt.date(2026, 10, 6), ZN=dt.date(2026, 10, 6), CL=dt.date(2026, 10, 7))
    out = _monday_continuous(env, "partial", STALE, 33, bars=bars)
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert out[DESK].action != ALERT


def test_one_day_more_than_x_is_stale(env):
    bars = {s: SUN for s in ROOTS36}
    bars.update({s: dt.date(2026, 10, 6) for s in STALE})
    out = _monday_continuous(env, "partial", STALE, 33, bars=bars)
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"


def test_the_alert_names_every_missing_symbol_the_stalest_first(env):
    """The row names them in its own order; the alert lists them with no bar first, then the oldest."""
    bars = {s: SUN for s in ROOTS36}
    del bars["CL"]
    bars.update(ES=FRI, ZN=dt.date(2026, 10, 2), GC=dt.date(2026, 10, 5))
    _monday_continuous(env, "partial", ("ES", "ZN", "GC", "CL"), 32, bars=bars, data_ready_max_missing=3)
    (message,) = president_messages(env)
    assert ("missing: CL (last bar none), ZN (last bar 2026-10-02), GC (last bar 2026-10-05), "
            "ES (last bar 2026-10-09). The books of 2026-10-12 are built") in message


def test_the_hold_names_the_stale_symbols_the_stalest_first(env):
    bars = {s: SUN for s in ROOTS36}
    bars.update(ZN=dt.date(2026, 10, 5), GC=dt.date(2026, 10, 2), CL=dt.date(2026, 10, 3))
    out = _monday_continuous(env, "partial", ("ZN", "GC", "CL"), 33, bars=bars)
    assert out[DESK].message.endswith(
        "GC (last bar 2026-10-02), CL (last bar 2026-10-03), ZN (last bar 2026-10-05). The books of "
        "2026-10-12 are not built")


def test_an_under_named_continuous_row_judges_the_unnamed_symbols_and_can_hold(env):
    """Q1 fix round 1 (audit A S1, ruling 21 to the letter): a partial row that misses more than it names
    (32 of 36, naming ES) breaks migration 028; the three it does not name are taken from the table, the
    stalest first (GC, ZN, CL, nine days old), and judged: three past X, more than N: held. The rule before
    ruling 21 ignored unnamed symbols and ran: the one kind of row on which Q1 adds a hold."""
    out = _monday_continuous(env, "partial", ("ES",), 32, bars=_stale(3, fresh=("ES",)))
    assert env.engine.calls == []
    assert out[DESK].reason == "data_stale"
    assert ("3 symbol(s) have had no bar for more than 4 day(s) as of 2026-10-11, more than "
            "QT_DATA_READY_MAX_MISSING=2: CL (last bar 2026-10-02), GC (last bar 2026-10-02), "
            "ZN (last bar 2026-10-02)") in out[DESK].message


def test_an_under_named_continuous_row_with_fresh_unnamed_symbols_runs_naming_them(env):
    """33 of 36, naming ES: the two unnamed are the table's stalest before the row's date (GC and ZN,
    Friday's), within X: the run goes, the alert naming all three, the stalest first."""
    bars = {s: SUN for s in ROOTS36}
    bars.update(ES=dt.date(2026, 10, 10), GC=FRI, ZN=FRI)
    _monday_continuous(env, "partial", ("ES",), 33, bars=bars)
    assert calls(env) == [("model", DESK, "2026-10-12")]
    (message,) = president_messages(env)
    assert ("missing: GC (last bar 2026-10-09), ZN (last bar 2026-10-09), ES (last bar 2026-10-10)."
            in message)


def test_an_under_named_row_the_table_cannot_fill_counts_the_rest_as_stale(env):
    """34 of 36 loaded, naming only ES: one more is missing, and no other symbol in the table is older than
    the row's date, so the unnamed one counts as stale; one is not more than N: runs, the alert says so."""
    _monday_continuous(env, "partial", ("ES",), 34, bars={s: SUN for s in ROOTS36} | {"ES": FRI})
    assert calls(env) == [("model", DESK, "2026-10-12")]
    (message,) = president_messages(env)
    assert "1 symbol(s) not named, with no bar in the table" in message


def test_the_continuous_n_setting_moves_the_rule(env):
    out = _monday_continuous(env, "partial", STALE[:1], 35, bars=_stale(1), data_ready_max_missing=0)
    assert env.engine.calls == []                                    # N = 0: one symbol past X holds
    assert "more than QT_DATA_READY_MAX_MISSING=0" in out[DESK].message
    assert catchup_settings({}).data_ready_max_missing == 2
    assert catchup_settings({"QT_DATA_READY_MAX_MISSING": "4"}).data_ready_max_missing == 4
    assert catchup_settings({"QT_DATA_READY_MAX_MISSING": "4"}).contract_data_ready_max_missing == 2
    with pytest.raises(ConfigError):
        catchup_settings({"QT_DATA_READY_MAX_MISSING": "two"})


def test_the_continuous_x_is_the_portfolios_tolerance(env):
    """X is live.data_staleness_tolerance_days as the engine reads it: raised to 10, nine days old runs."""
    (env.config / "defaults.json").write_text(json.dumps({"live": {"data_staleness_tolerance_days": 10}}))
    _monday_continuous(env, "partial", STALE, 33, bars=_stale(3))
    assert calls(env) == [("model", DESK, "2026-10-12")]


def test_a_held_contract_row_sends_no_continuous_partial_alert(env):
    """Both rows missing symbols: the continuous row would run, the per-contract row holds. Only the hold
    is alerted (the partial alerts go out once no row holds the run)."""
    switch_on(env)
    weekend_done(env)
    env.days.ready.append(ready(SUN, at(MON, 4, 35), "partial", ("ES",), 25, expected=26))
    env.days.bars = {"ES": FRI}
    env.days.ready.append(contract_ready(SUN, at(MON, 4, 36), "partial", STALE, loaded=33))
    env.days.contract_bars = {r: dt.date(2026, 10, 2) for r in STALE}
    env.clock.t = at(MON, 4, 40)
    out = dict(env.make(portfolios=(DESK,)).run_pass())
    assert out[DESK].reason == "data_stale" and env.engine.calls == []
    assert alerts(env) == [f"QT alert: {DESK_PID} 2026-10-12 data_stale"]


def test_both_rows_partial_and_running_alert_once_each(env):
    switch_on(env)
    weekend_done(env)
    env.days.ready.append(ready(SUN, at(MON, 4, 35), "partial", ("ES",), 25, expected=26))
    env.days.bars = {"ES": FRI}
    env.days.ready.append(contract_ready(SUN, at(MON, 4, 36), "partial", ("GC",), loaded=35))
    env.days.contract_bars = {"GC": FRI}
    env.clock.t = at(MON, 4, 40)
    env.make(portfolios=(DESK,)).run_pass()
    assert calls(env) == [("model", DESK, "2026-10-12")]
    assert sorted(alerts(env)) == [f"QT alert: {DESK_PID} 2026-10-12 contract_data_partial",
                                   f"QT alert: {DESK_PID} 2026-10-12 data_partial"]
    (_, body), = reviews(env, MON)
    assert "ES: last bar 2026-10-09" in body and "GC: last bar 2026-10-09" in body


def test_the_review_email_lists_a_failed_pulls_carried_symbols(env):
    """The desk's review of a run built from a failed pull lists every symbol carried (the row names
    none), each with its last bar, and says they are carried."""
    published_day(env, MON)
    env.days.ready.append(ready(MON, at(MON, 20, 10), "failed", loaded=0, expected=2))
    env.clock.t = at(MON, 20, 30)
    env.days.bars = {s: FRI for s in ("ES", "ZN")}
    env.make(portfolios=(DESK,)).run_pass()
    assert ("model", DESK, "2026-10-13") in calls(env)
    (_, body), = reviews(env)
    assert "2 symbol(s) are missing for 2026-10-12 (status failed)" in body
    assert "ES: last bar 2026-10-09" in body and "ZN: last bar 2026-10-09" in body
    assert "carried at its last price" in body
