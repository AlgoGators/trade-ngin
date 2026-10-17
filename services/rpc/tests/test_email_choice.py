"""D-66: the desk approves with or without the e-mail, and sends a published, unsent day later
(Send: a publish row with flow "send")."""

import datetime as dt

import pytest

from conftest import NY, PID, make_row
from algogators_rpc.services.desk.jobs import RunResult
from test_cutoff import FRI, approve, ny

UTC = dt.timezone.utc


@pytest.mark.parametrize("dispatched", [(9, 0), (9, 45)])
def test_an_approval_without_the_email_never_sends(agent, cstore, runner, dispatched):
    out, argvs = approve(agent, cstore, runner, FRI, ny(FRI, *dispatched), ny(FRI, *dispatched),
                         payload={"email": False})
    assert out.status == "accepted"
    assert argvs[0][3] == "--publish" and "--send-now" not in argvs[0]


def test_an_approval_with_the_email_sends_from_0930(agent, cstore, runner):
    out, argvs = approve(agent, cstore, runner, FRI, ny(FRI, 9, 45), ny(FRI, 9, 45),
                         payload={"email": True})
    assert argvs[0][-1] == "--send-now"


def published(cstore, day=FRI, sent_at=None):
    cstore.metadata[(PID, day)] = {"published_by": "desk@x", "published_at": ny(day, 9),
                                   "publish_source": "desk", "sent_at": sent_at}
    cstore.published[(PID, day)] = ny(day, 9)


def send_row(cstore, agent, day=FRI, now=None, audit_id=95):
    cstore.add(make_row(audit_id, "publish", date=day, payload={"flow": "send"}))
    agent.now = now or ny(FRI, 11)
    out = agent.dispatcher.dispatch(cstore.rows[audit_id])
    agent.wait()
    return out, cstore.rows[audit_id]


def test_send_emails_a_published_unsent_day_with_the_engines_send(agent, cstore, runner):
    published(cstore)

    def engine_sends(argv):
        cstore.metadata[(PID, FRI)]["sent_at"] = ny(FRI, 11, 1)
        return RunResult(rc=0, stdout="sent\n", stderr="")
    runner.behave = engine_sends
    out, row = send_row(cstore, agent)
    assert out.status == "accepted"
    argv = runner.calls[0][0]
    assert argv[2:] == [agent.settings.engine_binary, "--send", "--portfolio-config",
                        argv[5], "--date", "2026-10-09"] and "--audit-id" not in argv
    assert row.status == "done" and row.result["sent"] is True
    assert "published_at" not in row.result      # a Send never counts as an approval (C8)


@pytest.mark.parametrize("setup, now, reason", [
    (lambda c: published(c), ny(dt.date(2026, 10, 10), 8), "not_today"),
    (lambda c: None, ny(FRI, 11), "unpublished"),
    (lambda c: published(c, sent_at=ny(FRI, 9, 30)), ny(FRI, 11), "sent"),
])
def test_send_refusals_run_nothing(agent, cstore, runner, setup, now, reason):
    setup(cstore)
    _, row = send_row(cstore, agent, now=now)
    assert row.status == "refused" and row.result["reason"] == reason
    assert runner.calls == []


def test_a_send_the_engine_does_not_record_fails(agent, cstore, runner):
    published(cstore)
    runner.behave = lambda argv: RunResult(rc=1, stdout="", stderr="smtp down\n")
    _, row = send_row(cstore, agent)
    assert row.status == "failed" and "try again" in row.message
