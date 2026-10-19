"""C7 on the desk service: from 10:00 New York on its date a day takes no save or override request,
even while its fallback has not published it yet (a late or failed fallback). Like an approval, a
row counts by when the desk acted (its created_at). A decision from 10:00 is refused by its link's
expiry, which is 10:00 on the book date (D-13; test_commands)."""

import datetime as dt

import pytest

from conftest import DAY, NY, make_row


def at(hh, mm=0):
    return dt.datetime(DAY.year, DAY.month, DAY.day, hh, mm, tzinfo=NY)


@pytest.mark.parametrize("kind", ["save", "override_request"])
def test_a_desk_command_from_1000_is_refused_before_the_fallback(agent, cstore, runner, kind):
    cstore.add(make_row(70, kind, created_at=at(10, 1)))
    agent.now = at(10, 2)

    out = agent.dispatcher.dispatch(cstore.rows[70])
    agent.wait()

    assert out.status == "refused"
    assert cstore.rows[70].result == {"reason": "past_cutoff"}
    assert "10:00" in cstore.rows[70].message
    assert runner.calls == []


def test_a_save_pressed_before_1000_still_runs_after_it(agent, cstore, runner):
    cstore.add(make_row(71, "save", created_at=at(9, 59)))
    agent.now = at(10, 3)

    agent.dispatcher.dispatch(cstore.rows[71])
    agent.wait()

    assert [c[0][-1] for c in runner.calls] == ["71"]
