"""Rescind (contract C8; desk.proto 1.2.0): the desk takes back its own approval before the 09:30
send, end to end through gRPC against the in-memory command log (conftest.py)."""

import datetime as dt
import os

import grpc
import pytest

from conftest import DAY, NY, PID, make_row
from algogators import desk_pb2 as pb
from algogators import desk_pb2_grpc

DATE = DAY.isoformat()
APPROVED_AT = dt.datetime(2026, 10, 7, 12, 0, tzinfo=dt.timezone.utc)  # 08:00 New York


@pytest.fixture
def stub(channel):
    return desk_pb2_grpc.DeskServiceStub(channel)


def rescind(stub, audit_id, pid=PID, date=DATE):
    return stub.Rescind(pb.RescindRequest(portfolio_id=pid, date=date, audit_id=audit_id),
                        timeout=5)


def approved(cstore, source="desk", by="desk@algogators.com", sent_at=None):
    cstore.metadata[(PID, DAY)] = {"published_by": by, "published_at": APPROVED_AT,
                                   "publish_source": source, "sent_at": sent_at}
    cstore.published[(PID, DAY)] = APPROVED_AT


def test_rescind_clears_the_desk_approval_and_records_it(stub, agent, cstore, runner):
    approved(cstore)
    cstore.add(make_row(70, "rescind"))
    reply = rescind(stub, 70)
    assert reply.status == pb.COMMAND_STATUS_ACCEPTED, reply.message
    agent.wait()
    row = cstore.rows[70]
    assert row.status == "done", row.message
    assert row.result == {"rescinded": {"published_by": "desk@algogators.com",
                                        "published_at": APPROVED_AT.isoformat()}}
    assert "rescinded" in row.message and "09:30" in row.message
    assert cstore.metadata[(PID, DAY)]["published_at"] is None
    assert cstore.metadata[(PID, DAY)]["publish_source"] is None
    assert runner.calls == []  # no engine process: the agent does it in one transaction
    # Under the portfolio's flock, the one the engine jobs and the scheduler take.
    assert agent.locks == [(os.path.join(agent.settings.lock_dir, f"{PID}.lock"), 120.0)]


def test_a_rescinded_day_takes_a_new_approval(stub, agent, cstore, runner):
    approved(cstore)
    cstore.add(make_row(71, "rescind"))
    rescind(stub, 71)
    agent.wait()
    assert cstore.published_at(PID, DAY) is None
    cstore.add(make_row(72, "publish"))
    reply = stub.Publish(pb.PublishRequest(portfolio_id=PID, date=DATE, published_by="dom",
                                           audit_id=72), timeout=5)
    assert reply.status == pb.COMMAND_STATUS_ACCEPTED, reply.message
    agent.wait()
    assert [c[0][-1] for c in runner.calls] == ["72"]


@pytest.mark.parametrize("now, needle", [
    (dt.datetime(2026, 10, 7, 9, 30, tzinfo=NY), "send time has passed"),
    (dt.datetime(2026, 10, 7, 9, 45, tzinfo=NY), "send time has passed"),
    (dt.datetime(2026, 10, 8, 8, 0, tzinfo=NY), "send time has passed"),
])
def test_rescind_is_refused_from_0930(stub, agent, cstore, now, needle):
    approved(cstore)
    agent.now = now
    cstore.add(make_row(73, "rescind"))
    rescind(stub, 73)
    agent.wait()
    row = cstore.rows[73]
    assert row.status == "refused" and needle in row.message
    assert cstore.metadata[(PID, DAY)]["published_at"] == APPROVED_AT


def test_rescind_just_before_0930_is_taken(stub, agent, cstore):
    approved(cstore)
    agent.now = dt.datetime(2026, 10, 7, 9, 29, 59, tzinfo=NY)
    cstore.add(make_row(74, "rescind"))
    rescind(stub, 74)
    agent.wait()
    assert cstore.rows[74].status == "done"


@pytest.mark.parametrize("setup, needle", [
    (lambda c: None, "is not approved; nothing to rescind"),
    (lambda c: approved(c, source="fallback", by="system:fallback-10am"),
     "only the desk's own approval can be rescinded"),
    (lambda c: approved(c, source="model-only", by="system:model-only"),
     "only the desk's own approval can be rescinded"),
    (lambda c: approved(c, sent_at=dt.datetime(2026, 10, 7, 13, 30, tzinfo=dt.timezone.utc)),
     "a sent approval cannot be rescinded"),
])
def test_rescind_refusals_change_nothing(stub, agent, cstore, setup, needle):
    setup(cstore)
    before = dict(cstore.metadata.get((PID, DAY)) or {})
    cstore.add(make_row(75, "rescind"))
    rescind(stub, 75)
    agent.wait()
    row = cstore.rows[75]
    assert row.status == "refused" and needle in row.message, row.message
    assert (cstore.metadata.get((PID, DAY)) or {}) == before


def test_only_the_approver_can_rescind(stub, agent, cstore):
    approved(cstore, by="Desk@AlgoGators.com")
    cstore.add(make_row(81, "rescind", requested_by="other@algogators.com"))
    rescind(stub, 81)
    agent.wait()
    row = cstore.rows[81]
    assert row.status == "refused" and "only they can rescind" in row.message, row.message
    assert cstore.metadata[(PID, DAY)]["published_at"] == APPROVED_AT
    # The approver, whatever the case of the e-mail.
    cstore.add(make_row(82, "rescind", requested_by="desk@algogators.com"))
    rescind(stub, 82)
    agent.wait()
    assert cstore.rows[82].status == "done"


def test_rescind_waits_for_the_portfolio_lock_then_fails_untouched(stub, agent, cstore):
    approved(cstore)
    agent.lock_free = False
    cstore.add(make_row(76, "rescind"))
    rescind(stub, 76)
    agent.wait()
    row = cstore.rows[76]
    assert row.status == "failed" and "busy" in row.message
    assert cstore.metadata[(PID, DAY)]["published_at"] == APPROVED_AT


def test_rescind_is_idempotent_and_kind_checked(stub, agent, cstore):
    approved(cstore)
    cstore.add(make_row(77, "rescind"))
    rescind(stub, 77)
    agent.wait()
    again = rescind(stub, 77)
    assert "already done" in again.message
    cstore.add(make_row(78, "publish"))
    reply = rescind(stub, 78)
    assert reply.status == pb.COMMAND_STATUS_REFUSED and "not a rescind" in reply.message
    assert cstore.rows[78].status == "pending"


def test_rescind_must_match_its_row(stub, cstore):
    cstore.add(make_row(79, "rescind"))
    assert "not 2026-10-08" in rescind(stub, 79, date="2026-10-08").message
    assert "not OTHER" in rescind(stub, 79, pid="OTHER").message


@pytest.mark.parametrize("audit_id", [0, -1])
def test_rescind_needs_an_audit_id(stub, audit_id):
    with pytest.raises(grpc.RpcError) as err:
        rescind(stub, audit_id)
    assert err.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_a_pending_rescind_is_redriven(agent, cstore):
    from algogators_rpc.services.desk.recovery import sweep_pending

    approved(cstore)
    cstore.add(make_row(80, "rescind"))
    sweep_pending(cstore, agent.dispatcher)
    agent.wait()
    assert cstore.rows[80].status == "done"
