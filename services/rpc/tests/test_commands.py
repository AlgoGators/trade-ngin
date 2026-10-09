"""The four commands end to end through gRPC, against the in-memory command log and a fake
engine runner (conftest.py)."""

import datetime as dt
import hashlib
import logging
import re
import threading
import time

import pytest

from conftest import PDIR, PID, BookLine, make_row, now, write_portfolio
from algogators_rpc.services.desk.jobs import RunResult
from algogators_rpc.log import JsonFormatter
from algogators_rpc.services.desk.recovery import redrive_pending, sweep_pending
from algogators import desk_pb2 as pb
from algogators import desk_pb2_grpc

DATE = "2026-10-07"
BINARY = "/app/build/bin/Release/live_portfolio_conservative"


@pytest.fixture
def stub(channel):
    return desk_pb2_grpc.DeskServiceStub(channel)


def run_desk(stub, audit_id, pid=PID, date=DATE):
    return stub.RunDesk(pb.RunDeskRequest(portfolio_id=pid, date=date, audit_id=audit_id,
                                          requested_by="desk@algogators.com"), timeout=5)


def request_override(stub, audit_id):
    return stub.RequestOverride(pb.OverrideRequest(
        portfolio_id=PID, date=DATE, audit_id=audit_id, requested_by="desk@algogators.com",
        reason="roll early"), timeout=5)


def decide(stub, audit_id, token="tok-from-link"):
    return stub.RecordDecision(pb.DecisionRequest(audit_id=audit_id, approver="vp@algogators.com",
                                                  approved=True, token=token), timeout=5)


def rendered(caplog):
    return "\n".join(JsonFormatter().format(r) for r in caplog.records)


def expected_argv(agent, mode, audit_id):
    lock = agent.settings.lock_dir + "/" + PID + ".lock"
    return ["flock", lock.replace("\\", "/"), BINARY, f"--{mode}", "--portfolio-config", PDIR,
            "--date", DATE, "--audit-id", str(audit_id)]


def argv_of(call):
    argv = list(call[0])
    argv[1] = argv[1].replace("\\", "/")
    return argv


# -- RunDesk -------------------------------------------------------------------------------------

def test_run_desk_claims_and_spawns_the_engine(stub, agent, cstore, runner):
    cstore.add(make_row(10, "save"))
    reply = run_desk(stub, 10)
    assert reply.status == pb.COMMAND_STATUS_ACCEPTED
    assert reply.source == pb.BOOK_SOURCE_DESK
    agent.wait()
    assert cstore.claims == [10]
    assert len(runner.calls) == 1
    argv, cwd, timeout = runner.calls[0]
    assert argv_of(runner.calls[0]) == expected_argv(agent, "desk", 10)
    assert cwd == "/app" and timeout == 30
    row = cstore.rows[10]
    assert row.status == "done" and row.message == "engine said done"  # the engine's outcome


def test_run_desk_is_idempotent(stub, agent, cstore, runner):
    cstore.add(make_row(11, "save"))
    run_desk(stub, 11)
    agent.wait()
    again = run_desk(stub, 11)
    assert again.status == pb.COMMAND_STATUS_ACCEPTED
    assert "already done" in again.message
    agent.wait()
    assert len(runner.calls) == 1 and cstore.claims == [11]


@pytest.mark.parametrize("status", ["running", "refused", "failed"])
def test_run_desk_never_reruns_a_claimed_row(stub, agent, cstore, runner, status):
    # A young 'running' row after the startup recovery may be an engine started by hand; an
    # orphaned one is re-driven (test_hardening.py).
    agent.dispatcher.recovered = True
    cstore.add(make_row(12, "save", status=status, started_at=now()))
    reply = run_desk(stub, 12)
    assert reply.status == pb.COMMAND_STATUS_ACCEPTED and f"already {status}" in reply.message
    agent.wait()
    assert runner.calls == [] and cstore.rows[12].status == status


def test_kind_mismatch_refuses_and_leaves_the_row(stub, agent, cstore, runner):
    cstore.add(make_row(13, "publish"))
    reply = run_desk(stub, 13)
    assert reply.status == pb.COMMAND_STATUS_REFUSED
    assert "is a publish, not a save" in reply.message
    assert reply.source == pb.BOOK_SOURCE_UNSPECIFIED
    assert cstore.rows[13].status == "pending" and cstore.claims == []


@pytest.mark.parametrize("pid,date,needle", [("OTHER_PORTFOLIO", DATE, "not OTHER_PORTFOLIO"),
                                             (PID, "2026-10-06", "not 2026-10-06")])
def test_request_must_match_its_row(stub, cstore, pid, date, needle):
    cstore.add(make_row(14, "save"))
    reply = run_desk(stub, 14, pid=pid, date=date)
    assert reply.status == pb.COMMAND_STATUS_REFUSED and needle in reply.message
    assert cstore.rows[14].status == "pending"


def test_unknown_portfolio_config_refuses_the_row(stub, agent, cstore, runner):
    cstore.add(make_row(15, "save", portfolio_id="NO_SUCH_PORTFOLIO"))
    reply = run_desk(stub, 15, pid="NO_SUCH_PORTFOLIO")
    assert reply.status == pb.COMMAND_STATUS_REFUSED
    row = cstore.rows[15]
    assert row.status == "refused" and "no portfolio config" in row.message
    assert runner.calls == []


def test_ambiguous_portfolio_config_refuses_the_row(stub, agent, cstore, runner, config_dir):
    write_portfolio(config_dir, dirname="qt_conservative_copy")
    cstore.add(make_row(16, "save"))
    reply = run_desk(stub, 16)
    assert reply.status == pb.COMMAND_STATUS_REFUSED
    assert "qt_conservative, qt_conservative_copy" in cstore.rows[16].message
    assert runner.calls == []


def test_engine_exit_without_outcome_fails_the_row(stub, agent, cstore, runner):
    runner.behave = lambda argv: RunResult(rc=139, stdout="started\n",
                                           stderr="loading\nSegmentation fault\n")
    cstore.add(make_row(17, "save"))
    run_desk(stub, 17)
    agent.wait()
    row = cstore.rows[17]
    assert row.status == "failed"
    assert row.message == ("engine exited 139 without recording an outcome: "
                           "Segmentation fault")


def test_engine_outcome_is_not_overwritten(stub, agent, cstore, runner):
    runner.behave = runner.engine_records("refused", rc=2)
    cstore.add(make_row(18, "save"))
    run_desk(stub, 18)
    agent.wait()
    assert cstore.rows[18].status == "refused"
    assert cstore.rows[18].message == "engine said refused"


def test_engine_timeout_fails_the_row(stub, agent, cstore, runner):
    runner.behave = lambda argv: RunResult(rc=-9, timed_out=True)
    cstore.add(make_row(19, "save"))
    run_desk(stub, 19)
    agent.wait()
    assert cstore.rows[19].status == "failed"
    assert cstore.rows[19].message == "engine timed out after 30s and was killed"


def test_engine_that_cannot_start_fails_the_row(stub, agent, cstore, runner):
    runner.behave = lambda argv: RunResult(rc=None, error="FileNotFoundError: no such file")
    cstore.add(make_row(20, "save"))
    run_desk(stub, 20)
    agent.wait()
    assert cstore.rows[20].status == "failed"
    assert cstore.rows[20].message.startswith("cannot start the engine: FileNotFoundError")


def test_engine_output_tail_is_logged(stub, agent, cstore, runner, caplog):
    caplog.set_level(logging.INFO)
    lines = "".join(f"line {i}\n" for i in range(100))
    runner.behave = lambda argv: RunResult(rc=1, stdout=lines, stderr="boom\n")
    cstore.add(make_row(21, "save"))
    run_desk(stub, 21)
    agent.wait()
    rec = next(r for r in caplog.records if r.getMessage() == "engine exited")
    assert rec.rc == 1 and len(rec.stdout_tail) == 40 and rec.stdout_tail[-1] == "line 99"
    assert rec.stderr_tail == ["boom"]
    assert '"stdout_tail"' in JsonFormatter().format(rec)


# -- the per-portfolio queue -----------------------------------------------------------------------

def test_jobs_for_one_portfolio_run_one_at_a_time_in_order(agent, cstore, runner, config_dir):
    write_portfolio(config_dir, dirname="other", portfolio_id="OTHER")
    active, peak, order = {}, {}, []
    lock = threading.Lock()
    both_running = threading.Event()

    def behave(argv):
        pid = argv[1].replace("\\", "/").rsplit("/", 1)[-1][:-len(".lock")]
        with lock:
            active[pid] = active.get(pid, 0) + 1
            peak[pid] = max(peak.get(pid, 0), active[pid])
            order.append(int(argv[-1]))
            if len([p for p, n in active.items() if n]) == 2:
                both_running.set()
        time.sleep(0.05)
        with lock:
            active[pid] -= 1
        return runner.engine_records("done")(argv)

    runner.behave = behave
    for i in (30, 31, 32):
        cstore.add(make_row(i, "save"))
    cstore.add(make_row(33, "save", portfolio_id="OTHER"))
    for i in (30, 31, 32, 33):
        assert agent.dispatcher.dispatch(cstore.rows[i]).status == "accepted"
    agent.wait()
    assert peak[PID] == 1                                    # serialised per portfolio
    assert [i for i in order if i != 33] == [30, 31, 32]     # FIFO
    assert both_running.is_set()                             # portfolios run concurrently
    assert all(cstore.rows[i].status == "done" for i in (30, 31, 32, 33))


# -- RequestOverride -------------------------------------------------------------------------------

def override_row(id=40, **kw):
    return make_row(id, "override_request", reason="breach is temporary", **kw)


def test_override_with_email_disabled_writes_the_mail_into_result(stub, agent, cstore, runner,
                                                                  caplog):
    caplog.set_level(logging.DEBUG)
    cstore.book_data = {
        "system": {"ES": BookLine(3), "NQ": BookLine(-1)},
        "qt_proposal": {"ES": BookLine(5), "NQ": BookLine(-1)},
        "qt": {"ES": BookLine(4, "cap"), "NQ": BookLine(-1)},
    }
    cstore.add(override_row())
    reply = request_override(stub, 40)
    assert reply.status == pb.COMMAND_STATUS_ACCEPTED
    agent.wait()
    row = cstore.rows[40]
    assert row.status == "done"
    assert row.message == "e-mail disabled (QT_EMAIL_DISABLED=1): body logged in result"
    assert row.result["emailed"] == [] and row.result["email_disabled"] is True
    mail = row.result["email"]
    assert mail["to"] == ["vp@algogators.com", "pres@algogators.com"]
    assert mail["subject"] == f"QT override request: {PID} 2026-10-07"
    token = re.search(r"https://algolens\.algogators\.com/qt/approve\?token=([A-Za-z0-9_-]+)",
                      mail["body"]).group(1)
    assert len(token) >= 40
    assert row.token_hash == hashlib.sha256(token.encode()).hexdigest()
    left = row.token_expires_at - now()
    assert dt.timedelta(hours=47, minutes=59) < left <= dt.timedelta(hours=48)
    body = mail["body"]
    assert "desk@algogators.com" in body and "breach is temporary" in body
    assert "<td>ES</td><td align=\"right\">3</td><td align=\"right\">5</td>" \
           "<td align=\"right\">4</td><td>cap</td>" in body
    assert runner.calls == [] and agent.sent == []          # no binary, no SMTP
    assert token not in rendered(caplog)


def test_override_email_sent(stub, agent, cstore, settings, config_dir, caplog):
    caplog.set_level(logging.DEBUG)
    write_portfolio(config_dir, email={
        "smtp_host": "smtp.example.com", "smtp_port": 587, "username": "bot@example.com",
        "password": "app-password-123", "from_email": "qt@example.com", "use_tls": True})
    agent.settings = settings.__class__(**{**settings.__dict__, "email_disabled": False,
                                           "approve_url_base": "https://lens.test/approve"})
    agent.dispatcher._settings = agent.settings
    cstore.add(override_row())
    request_override(stub, 40)
    agent.wait()
    row = cstore.rows[40]
    assert row.status == "done" and row.result["emailed"] == ["vp", "president"]
    assert dt.datetime.fromisoformat(row.result["emailed_at"]) <= now()
    assert row.message == "override e-mail sent to vp and president"
    cfg, to, subject, body = agent.sent[0]
    assert to == ["vp@algogators.com", "pres@algogators.com"]
    assert cfg.host == "smtp.example.com" and cfg.from_email == "qt@example.com"
    token = re.search(r"https://lens\.test/approve\?token=([A-Za-z0-9_-]+)", body).group(1)
    assert row.token_hash == hashlib.sha256(token.encode()).hexdigest()
    log = rendered(caplog)
    assert token not in log and "app-password-123" not in log


def test_override_smtp_failure_fails_the_row(stub, agent, cstore, settings, config_dir):
    write_portfolio(config_dir, email={"smtp_host": "h", "username": "u", "password": "p",
                                       "use_tls": False})
    agent.dispatcher._settings = settings.__class__(**{**settings.__dict__,
                                                       "email_disabled": False})

    def broken(*a):
        raise ConnectionRefusedError("refused")
    agent.dispatcher._sender = broken
    cstore.add(override_row())
    request_override(stub, 40)
    agent.wait()
    assert cstore.rows[40].status == "failed"
    assert cstore.rows[40].message == "override e-mail failed: ConnectionRefusedError"


def test_override_without_approvers_fails(stub, agent, cstore, settings):
    agent.dispatcher._settings = settings.__class__(**{**settings.__dict__,
                                                       "approvers": "vp=vp@x.com"})
    cstore.add(override_row())
    request_override(stub, 40)
    agent.wait()
    row = cstore.rows[40]
    assert row.status == "failed" and "QT_APPROVERS" in row.message and "president" in row.message
    assert row.token_hash is None


def test_override_kind_mismatch(stub, cstore):
    cstore.add(make_row(41, "save"))
    reply = request_override(stub, 41)
    assert reply.status == pb.COMMAND_STATUS_REFUSED and "not a override_request" in reply.message


# -- RecordDecision --------------------------------------------------------------------------------

def requested(cstore, id=50, expires_in=dt.timedelta(hours=40), **kw):
    return cstore.add(make_row(id, "override_request", status="done", token_hash="ab" * 32,
                               token_expires_at=now() + expires_in, **kw))


def decision(cstore, id=51, parent=50, approved=True, by="vp@algogators.com", **kw):
    return cstore.add(make_row(id, "override_decision", parent_id=parent, approver_role="vp",
                               requested_by=by, payload={"approved": approved}, reason=None,
                               **kw))


def test_requester_may_not_approve_their_own_request(stub, agent, cstore, runner):
    requested(cstore)
    decision(cstore, by="  Desk@AlgoGators.com ")
    reply = decide(stub, 51)
    assert reply.status == pb.COMMAND_STATUS_REFUSED
    assert reply.message == "the requester may not approve their own request"
    assert cstore.rows[51].status == "refused"
    assert cstore.rows[51].message == "the requester may not approve their own request"
    agent.wait()
    assert runner.calls == []


def test_rejection_is_done_without_the_engine(stub, agent, cstore, runner):
    requested(cstore)
    decision(cstore, approved=False)
    reply = decide(stub, 51)
    assert reply.status == pb.COMMAND_STATUS_DONE
    row = cstore.rows[51]
    assert row.status == "done" and row.result == {"approved": False}
    agent.wait()
    assert runner.calls == []


def test_approval_runs_the_engine_with_the_decision_row(stub, agent, cstore, runner):
    requested(cstore)
    decision(cstore)
    reply = decide(stub, 51)
    assert reply.status == pb.COMMAND_STATUS_ACCEPTED
    agent.wait()
    assert [argv_of(c) for c in runner.calls] == [expected_argv(agent, "override", 51)]
    assert cstore.rows[51].status == "done"


def test_expired_link_refuses(stub, agent, cstore, runner):
    requested(cstore, expires_in=-dt.timedelta(minutes=1))
    decision(cstore)
    reply = decide(stub, 51)
    assert reply.status == pb.COMMAND_STATUS_REFUSED and "expired" in reply.message
    agent.wait()
    assert runner.calls == []


@pytest.mark.parametrize("change,needle", [
    ({"parent": 999}, "is not an override request"),
    ({"approved": "yes"}, "payload.approved"),
    ({"approver_role": "cfo"}, "approver_role"),
])
def test_decision_checks(stub, cstore, change, needle):
    requested(cstore)
    kw = dict(change)
    role = kw.pop("approver_role", None)
    row = decision(cstore, **kw)
    if role:
        cstore.rows[51] = row.__class__(**{**row.__dict__, "approver_role": role})
    reply = decide(stub, 51)
    assert reply.status == pb.COMMAND_STATUS_REFUSED and needle in reply.message


def test_decision_on_a_request_without_token(stub, cstore):
    cstore.add(make_row(50, "override_request", status="failed"))
    decision(cstore)
    reply = decide(stub, 51)
    assert reply.status == pb.COMMAND_STATUS_REFUSED and "no approval token" in reply.message


def test_decision_token_never_logged_on_approval(stub, agent, cstore, caplog):
    caplog.set_level(logging.DEBUG)
    requested(cstore)
    decision(cstore)
    decide(stub, 51, token="very-secret-link-token")
    agent.wait()
    assert caplog.records and "very-secret-link-token" not in rendered(caplog)


# -- Publish ---------------------------------------------------------------------------------------

def publish(stub, audit_id=0):
    return stub.Publish(pb.PublishRequest(portfolio_id=PID, date=DATE, published_by="dom",
                                          audit_id=audit_id), timeout=5)


def test_publish_by_audit_id(stub, agent, cstore, runner):
    cstore.add(make_row(60, "publish"))
    assert publish(stub, 60).status == pb.COMMAND_STATUS_ACCEPTED
    agent.wait()
    assert [argv_of(c) for c in runner.calls] == [expected_argv(agent, "publish", 60)]


def test_publish_without_audit_id_takes_the_newest_pending(stub, agent, cstore, runner):
    cstore.add(make_row(61, "publish"))
    cstore.add(make_row(62, "publish"))
    cstore.add(make_row(63, "publish", date=dt.date(2026, 10, 8)))
    assert publish(stub).status == pb.COMMAND_STATUS_ACCEPTED
    agent.wait()
    assert [c[0][-1] for c in runner.calls] == ["62"]


def test_publish_without_a_row_refuses(stub, runner):
    reply = publish(stub)
    assert reply.status == pb.COMMAND_STATUS_REFUSED and "no pending publish row" in reply.message


def test_publish_negative_audit_id_is_invalid(stub):
    import grpc
    with pytest.raises(grpc.RpcError) as err:
        publish(stub, -5)
    assert err.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_store_down_is_unavailable(stub, cstore):
    import grpc
    cstore.error = "database error (OperationalError)"
    with pytest.raises(grpc.RpcError) as err:
        run_desk(stub, 10)
    assert err.value.code() == grpc.StatusCode.UNAVAILABLE


# -- re-drive --------------------------------------------------------------------------------------

def test_redrive_resets_orphans_and_dispatches_by_kind(agent, cstore, runner):
    cstore.add(make_row(70, "save", status="running", started_at=now()))   # orphan
    cstore.add(make_row(71, "override_request"))
    requested(cstore, id=72)
    decision(cstore, id=73, parent=72)
    cstore.add(make_row(74, "publish"))
    cstore.add(make_row(75, "save", status="done"))                       # left alone
    count = redrive_pending(cstore, agent.dispatcher)
    agent.wait()
    assert count == 4
    assert cstore.claims == [70, 71, 73, 74]
    modes = {c[0][-1]: c[0][3] for c in runner.calls}
    assert modes == {"70": "--desk", "73": "--override", "74": "--publish"}
    assert cstore.rows[71].status == "done" and cstore.rows[71].result["email_disabled"]
    assert cstore.rows[75].status == "done" and 75 not in cstore.claims


def test_redrive_never_double_dispatches(agent, cstore, runner):
    cstore.add(make_row(80, "save"))
    sweep_pending(cstore, agent.dispatcher)
    sweep_pending(cstore, agent.dispatcher)
    agent.wait()
    sweep_pending(cstore, agent.dispatcher)
    agent.wait()
    assert len(runner.calls) == 1 and cstore.claims == [80]


def test_redrive_survives_a_store_error(agent, cstore):
    cstore.error = "database error (OperationalError)"
    assert redrive_pending(cstore, agent.dispatcher) == 0
