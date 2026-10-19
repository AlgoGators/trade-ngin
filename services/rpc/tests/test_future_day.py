"""A future day's book is approved, or an override requested, as soon as its model run has
written it (Dominick 2026-10-18, replacing D-43's wait for the day itself); edits are taken as
before."""

import datetime as dt

import pytest

from conftest import DAY, PID, make_row
from test_override_margin import WITHIN
from algogators import desk_pb2 as pb
from algogators import desk_pb2_grpc

TOMORROW = DAY + dt.timedelta(days=1)


@pytest.fixture
def stub(channel):
    return desk_pb2_grpc.DeskServiceStub(channel)


def test_approving_a_future_day_runs_the_engines_publish(stub, agent, cstore, runner):
    cstore.add(make_row(60, "publish", date=TOMORROW))
    reply = stub.Publish(pb.PublishRequest(portfolio_id=PID, date=TOMORROW.isoformat(),
                                           published_by="desk@algogators.com", audit_id=60),
                         timeout=5)
    assert reply.status == pb.COMMAND_STATUS_ACCEPTED, reply.message
    agent.wait()
    assert [c[0][-1] for c in runner.calls] == ["60"]


def test_an_override_request_for_a_future_day_is_mailed(stub, agent, cstore):
    cstore.add(make_row(61, "override_request", date=TOMORROW, payload=WITHIN))
    stub.RequestOverride(pb.OverrideRequest(portfolio_id=PID, date=TOMORROW.isoformat(),
                                            audit_id=61, requested_by="desk@algogators.com",
                                            reason="r"), timeout=5)
    agent.wait()
    assert cstore.rows[61].status == "done", cstore.rows[61].message


def test_a_save_on_a_future_day_still_runs(stub, agent, cstore, runner):
    cstore.add(make_row(62, "save", date=TOMORROW))
    stub.RunDesk(pb.RunDeskRequest(portfolio_id=PID, date=TOMORROW.isoformat(), audit_id=62,
                                   requested_by="desk@algogators.com"), timeout=5)
    agent.wait()
    assert [c[0][-1] for c in runner.calls] == ["62"]
