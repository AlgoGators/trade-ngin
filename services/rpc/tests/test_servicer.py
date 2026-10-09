import datetime as dt
import logging

import grpc
import pytest
from grpc_health.v1 import health_pb2, health_pb2_grpc

from conftest import UTC
from algogators_rpc.services.desk import SERVICE_NAME
from algogators_rpc.log import JsonFormatter
from algogators_rpc.services.desk.store import MetadataRow, RunFacts
from algogators import desk_pb2 as pb
from algogators import desk_pb2_grpc

DAY = dt.date(2026, 10, 7)
PID = "CONSERVATIVE_PORTFOLIO"
T0 = dt.datetime(2026, 10, 7, 13, 30, tzinfo=UTC)
T1 = dt.datetime(2026, 10, 7, 13, 41, tzinfo=UTC)


@pytest.fixture
def stub(channel):
    return desk_pb2_grpc.DeskServiceStub(channel)


def status(stub, pid=PID, date="2026-10-07"):
    return stub.GetRunStatus(pb.RunStatusRequest(portfolio_id=pid, date=date), timeout=5)


# -- health ------------------------------------------------------------------------------------

@pytest.mark.parametrize("service", ["", SERVICE_NAME])
def test_health_serving(channel, service):
    reply = health_pb2_grpc.HealthStub(channel).Check(
        health_pb2.HealthCheckRequest(service=service), timeout=5)
    assert reply.status == health_pb2.HealthCheckResponse.SERVING


# -- GetRunStatus -------------------------------------------------------------------------------

def test_run_status_not_found(stub, store):
    reply = status(stub)
    assert reply.state == pb.RUN_STATE_NOT_STARTED
    assert "no trading.live_run_metadata row" in reply.message
    assert not reply.HasField("started_at") and not reply.HasField("finished_at")
    assert not reply.published
    assert store.calls == [(PID, DAY)]


def test_run_status_succeeded(stub, store):
    store.facts[(PID, DAY)] = RunFacts(
        metadata=(MetadataRow({"portfolio_id": PID}, created_at=T0),),
        has_results=True, results_written_at=T1)
    reply = status(stub)
    assert reply.state == pb.RUN_STATE_SUCCEEDED
    assert reply.started_at.ToDatetime(tzinfo=UTC) == T0
    assert reply.finished_at.ToDatetime(tzinfo=UTC) == T1


def test_run_status_running_until_results_row(stub, store):
    store.facts[(PID, DAY)] = RunFacts(metadata=(MetadataRow({}, created_at=T0),))
    reply = status(stub)
    assert reply.state == pb.RUN_STATE_RUNNING
    assert reply.HasField("started_at") and not reply.HasField("finished_at")


def test_run_status_risk_refusal_mark(stub, store):
    mark = {"action": "REFUSE", "module": "carver", "scope_id": PID, "phase": "pre",
            "reason": "leverage 5.1 over 4.0"}
    store.facts[(PID, DAY)] = RunFacts(
        metadata=(MetadataRow({"risk_refusal": mark}, created_at=T0),), has_results=True)
    reply = status(stub)
    assert reply.state == pb.RUN_STATE_REFUSED
    assert "leverage 5.1 over 4.0" in reply.message and "carver" in reply.message


def test_run_status_strict_assertion_wins_over_refusal(stub, store):
    cfg = {"risk_refusal": {"reason": "x"},
           "strict_assertion": {"reason": "book change(s) with no T-1 price",
                                "unpriced_book_changes": ["ES"]}}
    store.facts[(PID, DAY)] = RunFacts(metadata=(MetadataRow(cfg),))
    reply = status(stub)
    assert reply.state == pb.RUN_STATE_FAILED
    assert "no T-1 price" in reply.message


def test_run_status_mark_on_any_strategy_row(stub, store):
    # One row per strategy_id; a mark on any of them marks the day (the watchdog's bool_or).
    store.facts[(PID, DAY)] = RunFacts(
        metadata=(MetadataRow({}, created_at=T1),
                  MetadataRow({"risk_refusal": {}}, created_at=T0)),
        has_results=True)
    reply = status(stub)
    assert reply.state == pb.RUN_STATE_REFUSED
    assert reply.started_at.ToDatetime(tzinfo=UTC) == T0


def test_run_status_portfolio_config_as_text(stub, store):
    store.facts[(PID, DAY)] = RunFacts(
        metadata=(MetadataRow('{"risk_refusal": {"reason": "held"}}'),), has_results=True)
    assert status(stub).state == pb.RUN_STATE_REFUSED


def test_run_status_publish_columns_missing(stub, store):
    # Before the publish migration the columns do not exist: the reply says "not published",
    # whatever a store hands back.
    store.facts[(PID, DAY)] = RunFacts(
        metadata=(MetadataRow({}, published_by="dom", published_at=T1),),
        has_publish_columns=False, has_results=True)
    reply = status(stub)
    assert reply.state == pb.RUN_STATE_SUCCEEDED
    assert not reply.published and reply.published_by == ""
    assert not reply.HasField("published_at")


def test_run_status_published(stub, store):
    store.facts[(PID, DAY)] = RunFacts(
        metadata=(MetadataRow({}, published_by="dom", published_at=T1),),
        has_publish_columns=True, has_results=True)
    reply = status(stub)
    assert reply.published and reply.published_by == "dom"
    assert reply.published_at.ToDatetime(tzinfo=UTC) == T1


def test_run_status_publish_columns_present_not_published(stub, store):
    store.facts[(PID, DAY)] = RunFacts(metadata=(MetadataRow({}),), has_publish_columns=True,
                                       has_results=True)
    reply = status(stub)
    assert not reply.published and not reply.HasField("published_at")


def test_run_status_db_error_is_unavailable(stub, store):
    store.error = "database error (OperationalError)"
    with pytest.raises(grpc.RpcError) as err:
        status(stub)
    assert err.value.code() == grpc.StatusCode.UNAVAILABLE
    assert "OperationalError" in err.value.details()


@pytest.mark.parametrize("pid,date", [
    ("", "2026-10-07"), ("BAD ID;--", "2026-10-07"), (PID, ""), (PID, "2026-13-01"),
    (PID, "07/10/2026"), (PID, "2026-02-30")])
def test_run_status_invalid_argument(stub, store, pid, date):
    with pytest.raises(grpc.RpcError) as err:
        status(stub, pid, date)
    assert err.value.code() == grpc.StatusCode.INVALID_ARGUMENT
    assert store.calls == []


# -- commands: input validation, and a call with no row behind it ------------------------------

GOOD = {
    "RunDesk": pb.RunDeskRequest(portfolio_id=PID, date="2026-10-07", audit_id=42,
                                 requested_by="desk@algogators"),
    "RequestOverride": pb.OverrideRequest(portfolio_id=PID, date="2026-10-07", audit_id=43,
                                          requested_by="desk@algogators", reason="roll early"),
    "RecordDecision": pb.DecisionRequest(audit_id=43, approver="vp@algogators", approved=True,
                                         token="s3cret-token"),
    "Publish": pb.PublishRequest(portfolio_id=PID, date="2026-10-07", published_by="desk"),
}


@pytest.mark.parametrize("rpc", sorted(GOOD))
def test_commands_without_their_row_refuse(stub, rpc):
    # No position_overrides row behind the call: refused with the reason, never a fake success.
    reply = getattr(stub, rpc)(GOOD[rpc], timeout=5)
    assert reply.status == pb.COMMAND_STATUS_REFUSED
    assert "position_overrides" in reply.message
    if rpc == "RunDesk":
        assert len(reply.outcomes) == 0
        assert reply.source == pb.BOOK_SOURCE_UNSPECIFIED


def _with(msg, **changes):
    out = type(msg)()
    out.CopyFrom(msg)
    for k, v in changes.items():
        setattr(out, k, v)
    return out


INVALID = [
    ("RunDesk", {"audit_id": 0}), ("RunDesk", {"audit_id": -1}), ("RunDesk", {"date": "x"}),
    ("RunDesk", {"portfolio_id": ""}), ("RunDesk", {"requested_by": "  "}),
    ("RequestOverride", {"reason": ""}), ("RequestOverride", {"audit_id": 0}),
    ("RequestOverride", {"requested_by": ""}), ("RequestOverride", {"date": "2026-10-32"}),
    ("RecordDecision", {"token": ""}), ("RecordDecision", {"approver": ""}),
    ("RecordDecision", {"audit_id": 0}),
    ("Publish", {"published_by": ""}), ("Publish", {"portfolio_id": "a b"}),
    ("Publish", {"date": ""}),
]


@pytest.mark.parametrize("rpc,changes", INVALID)
def test_commands_validate_input(stub, rpc, changes):
    with pytest.raises(grpc.RpcError) as err:
        getattr(stub, rpc)(_with(GOOD[rpc], **changes), timeout=5)
    assert err.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_decision_token_never_logged(stub, caplog):
    caplog.set_level(logging.DEBUG)
    stub.RecordDecision(GOOD["RecordDecision"], timeout=5)
    with pytest.raises(grpc.RpcError):
        stub.RecordDecision(_with(GOOD["RecordDecision"], approver=""), timeout=5)
    rendered = "\n".join(JsonFormatter().format(r) for r in caplog.records)
    assert caplog.records and "s3cret-token" not in rendered
