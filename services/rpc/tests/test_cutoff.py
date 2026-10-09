"""Contract C7 on the desk service: the Publish command is the desk's approval. Before 09:30 New
York it only freezes the day (engine --publish); from 09:30 to 10:00 it also sends (--send-now);
a row created at or after 10:00 on its date, or on a published day, is refused. A scheduler
fallback row re-driven here runs engine --fallback. GetRunStatus reports publish_source and
sent_at (desk.proto 1.1.0)."""

import datetime as dt

import pytest

from conftest import NY, PID, make_row
from algogators import desk_pb2 as pb
from algogators.versions import API_VERSIONS
from algogators_rpc.services.desk import cutoff
from algogators_rpc.services.desk.store import MetadataRow, RunFacts

UTC = dt.timezone.utc
FRI, SAT = dt.date(2026, 10, 9), dt.date(2026, 10, 10)


def ny(day, hh, mm=0):
    return dt.datetime(day.year, day.month, day.day, hh, mm, tzinfo=NY)


def approve(agent, cstore, runner, day, created, dispatched, audit_id=90, **kw):
    cstore.add(make_row(audit_id, "publish", date=day, created_at=created, **kw))
    agent.now = dispatched
    out = agent.dispatcher.dispatch(cstore.rows[audit_id])
    agent.wait()
    return out, [c[0] for c in runner.calls]


@pytest.mark.parametrize("created,dispatched,send_now", [
    ((9, 29), (9, 29), False),      # approve only; the 09:30 send e-mails it
    ((9, 29), (9, 30), True),       # queued at 09:29, run at 09:30: sent at once
    ((9, 30), (9, 30), True),
    ((9, 59), (9, 59), True),
    ((9, 59), (10, 7), True),       # clicked before 10:00, run after: still the desk's
])
def test_an_approval_before_1000_is_accepted(agent, cstore, runner, created, dispatched,
                                             send_now):
    out, argvs = approve(agent, cstore, runner, FRI, ny(FRI, *created), ny(FRI, *dispatched))
    assert out.status == "accepted"
    assert argvs[0][3] == "--publish" and "90" in argvs[0]
    assert ("--send-now" in argvs[0]) is send_now


@pytest.mark.parametrize("created", [(10, 0), (10, 7), (15, 0)])
def test_an_approval_from_1000_is_refused(agent, cstore, runner, created):
    out, argvs = approve(agent, cstore, runner, FRI, ny(FRI, *created), ny(FRI, *created))
    assert out.status == "refused" and "deadline" in out.message and "10:00" in out.message
    assert argvs == [] and cstore.rows[90].status == "refused"


def test_an_approval_of_an_earlier_day_is_refused(agent, cstore, runner):
    out, argvs = approve(agent, cstore, runner, FRI, ny(SAT, 8), ny(SAT, 8))
    assert out.status == "refused" and argvs == []


def test_a_weekend_day_has_the_same_cutoff(agent, cstore, runner):
    out, argvs = approve(agent, cstore, runner, SAT, ny(SAT, 9, 45), ny(SAT, 9, 45))
    assert out.status == "accepted" and argvs[0][-1] == "--send-now"
    out, _ = approve(agent, cstore, runner, SAT, ny(SAT, 10, 0), ny(SAT, 10, 0), audit_id=91)
    assert out.status == "refused"


@pytest.mark.parametrize("day,utc_hh", [(dt.date(2026, 11, 1), 15), (dt.date(2026, 3, 8), 14)])
def test_the_cutoff_is_new_york_time_on_a_dst_day(agent, cstore, runner, day, utc_hh):
    before = dt.datetime(day.year, day.month, day.day, utc_hh - 1, 59, tzinfo=UTC)
    out, _ = approve(agent, cstore, runner, day, before, before)
    assert out.status == "accepted"
    at_cutoff = dt.datetime(day.year, day.month, day.day, utc_hh, 0, tzinfo=UTC)
    out, _ = approve(agent, cstore, runner, day, at_cutoff, at_cutoff, audit_id=91)
    assert out.status == "refused"
    assert cutoff.cutoff_time(day).astimezone(UTC).hour == utc_hh


def test_a_published_day_refuses_the_approval(agent, cstore, runner):
    cstore.published[(PID, FRI)] = ny(FRI, 10, 0)
    out, argvs = approve(agent, cstore, runner, FRI, ny(FRI, 9), ny(FRI, 9))
    assert out.status == "refused" and "already published" in out.message and argvs == []


@pytest.mark.parametrize("requested_by,dispatched,send_now", [
    ("system:fallback-10am", ny(FRI, 10, 3), True),       # today's fallback, re-driven
    ("system:fallback-10am", ny(SAT, 7, 0), False),       # a day later: never e-mailed
    ("system:fallback-catchup", ny(FRI, 11, 0), False),
])
def test_a_redriven_fallback_row_runs_the_fallback(agent, cstore, runner, requested_by,
                                                   dispatched, send_now):
    cstore.published[(PID, FRI)] = None
    out, argvs = approve(agent, cstore, runner, FRI, ny(FRI, 10), dispatched,
                         requested_by=requested_by)
    assert out.status == "accepted"
    assert argvs[0][3] == "--fallback" and ("--send-now" in argvs[0]) is send_now


def test_the_windows():
    assert cutoff.window(FRI, ny(FRI, 9, 29)) == cutoff.BEFORE_SEND
    assert cutoff.window(FRI, ny(FRI, 9, 30)) == cutoff.SEND_NOW
    assert cutoff.window(FRI, ny(FRI, 9, 59)) == cutoff.SEND_NOW
    assert cutoff.window(FRI, ny(FRI, 10, 0)) == cutoff.CLOSED
    assert cutoff.window(FRI, ny(FRI, 10, 7)) == cutoff.CLOSED
    assert cutoff.is_fallback("system:fallback-10am") and not cutoff.is_fallback("dom@x")


# -- GetRunStatus 1.1.0 ------------------------------------------------------------------------

def test_desk_proto_is_1_1_0():
    assert API_VERSIONS["desk"] == "1.1.0"
    fields = pb.RunStatus.DESCRIPTOR.fields_by_name
    assert fields["publish_source"].number == 8 and fields["sent_at"].number == 9
    assert fields["published_at"].number == 7 and fields["published"].number == 5


@pytest.fixture
def stub(channel):
    from algogators import desk_pb2_grpc
    return desk_pb2_grpc.DeskServiceStub(channel)


def status(stub):
    return stub.GetRunStatus(pb.RunStatusRequest(portfolio_id=PID, date="2026-10-09"), timeout=5)


@pytest.mark.parametrize("source,sent", [("desk", None), ("desk", 1), ("fallback", 1),
                                         ("model-only", None)])
def test_run_status_reports_approval_send_and_fallback(stub, store, source, sent):
    published = dt.datetime(2026, 10, 9, 13, 5, tzinfo=UTC)
    sent_at = dt.datetime(2026, 10, 9, 13, 30, tzinfo=UTC) if sent else None
    store.facts[(PID, FRI)] = RunFacts(
        metadata=(MetadataRow({}, published_by="dom", published_at=published,
                              publish_source=source, sent_at=sent_at),),
        has_publish_columns=True, has_results=True)
    reply = status(stub)
    assert reply.published and reply.publish_source == source
    assert reply.HasField("sent_at") is bool(sent)
    if sent:
        assert reply.sent_at.ToDatetime(tzinfo=UTC) == sent_at


def test_run_status_before_027_has_no_source(stub, store):
    store.facts[(PID, FRI)] = RunFacts(metadata=(MetadataRow({}),), has_publish_columns=True,
                                       has_results=True)
    reply = status(stub)
    assert reply.publish_source == "" and not reply.HasField("sent_at")
