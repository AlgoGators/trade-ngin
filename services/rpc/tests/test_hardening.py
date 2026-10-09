"""The 2026-10-09 hardening of the desk service (QT hardening spec, F-RPC items 1-4; contract
C1-C4): stale and orphaned 'running' rows, one decision per request, published days, the
override e-mail re-drive, bounded engine output and a health port of its own."""

import dataclasses
import datetime as dt
import hashlib
import os
import re
import shutil
import stat
import threading
import time

import grpc
import pytest
from grpc_health.v1 import health_pb2, health_pb2_grpc

from conftest import DISPATCH_AT, PID, FakeStore, make_row, now, write_portfolio
from algogators import desk_pb2 as pb
from algogators import desk_pb2_grpc
from algogators_rpc.client import versioned_channel
from algogators_rpc.registry import Registry
from algogators_rpc.server import build_health_server, build_server, run_start_hooks
from algogators_rpc.services.desk.config import (CONNECT_TIMEOUT_S, DbConfig, catchup_settings,
                                                 ConfigError)
from algogators_rpc.services.desk.jobs import TAIL_BYTES, RunResult, SubprocessRunner
from algogators_rpc.services.desk.recovery import (RESTART_MESSAGE, STALE_MESSAGE, redrive,
                                                   sweep_pending)
from algogators_rpc.services.desk.service import build_service
from algogators_rpc.services.desk.store import RunFacts

DATE = "2026-10-07"
DAY = dt.date(2026, 10, 7)


@pytest.fixture
def stub(channel):
    return desk_pb2_grpc.DeskServiceStub(channel)


def run_desk(stub, audit_id):
    return stub.RunDesk(pb.RunDeskRequest(portfolio_id=PID, date=DATE, audit_id=audit_id,
                                          requested_by="desk@algogators.com"), timeout=5)


def decide(stub, audit_id):
    return stub.RecordDecision(pb.DecisionRequest(audit_id=audit_id, approver="vp@algogators.com",
                                                  approved=True, token="t"), timeout=5)


def old(minutes):
    return now() - dt.timedelta(minutes=minutes)


# -- 1. stale and orphaned running rows --------------------------------------------------------

def test_restart_with_the_database_down_then_up(agent, cstore, runner, store):
    """The previous process left rows 'running'. The startup recovery fails (DB down) and is
    retried by the periodic task until it succeeds; then the rows are re-driven."""
    cstore.add(make_row(1, "save", status="running", started_at=old(2)))
    cstore.add(make_row(2, "publish", status="running", started_at=old(90)))
    cstore.add(make_row(3, "save"))
    service = build_service(store, agent.dispatcher, cstore)
    task = service.background_tasks[0]
    reg = Registry()
    reg.register(service)

    cstore.error = "database error (OperationalError)"
    run_start_hooks(reg)                      # the startup pass: fails, logged, not raised
    assert agent.dispatcher.recovered is False
    task.run()                                # still down: the periodic pass retries
    assert agent.dispatcher.recovered is False
    assert [r.status for r in cstore.rows.values()] == ["running", "running", "pending"]

    cstore.error = None                       # the database is back
    task.run()
    agent.wait()
    assert agent.dispatcher.recovered is True
    assert cstore.requeues == [1, 2]          # every orphan, however young
    assert cstore.claims == [1, 2, 3]
    assert {c[0][-1] for c in runner.calls} == {"1", "2", "3"}
    assert all(r.status == "done" for r in cstore.rows.values())


def test_startup_recovery_message(agent, cstore):
    cstore.add(make_row(1, "save", status="running", started_at=old(1)))
    cstore.fail["claim"] = 1                  # keep it pending after the requeue
    redrive(cstore, agent.dispatcher)
    assert cstore.rows[1].message == RESTART_MESSAGE


def test_nothing_runs_before_the_startup_recovery(agent, cstore, runner):
    cstore.add(make_row(3, "save"))
    cstore.fail["running_rows"] = 1
    assert redrive(cstore, agent.dispatcher) == 0
    assert cstore.claims == [] and runner.calls == []
    assert redrive(cstore, agent.dispatcher) == 1


def test_periodic_task_requeues_only_stale_unowned_rows(agent, cstore, runner, settings):
    agent.dispatcher.recovered = True
    limit = settings.job_timeout_s + settings.stale_grace_s
    cstore.add(make_row(1, "save", status="running",
                        started_at=now() - dt.timedelta(seconds=limit + 60)))
    cstore.add(make_row(2, "save", status="running",
                        started_at=now() - dt.timedelta(seconds=limit - 60)))
    redrive(cstore, agent.dispatcher)
    agent.wait()
    assert cstore.requeues == [1]
    assert cstore.rows[1].status == "done" and cstore.rows[2].status == "running"
    assert [c[0][-1] for c in runner.calls] == ["1"]


def test_requeue_message_for_a_stale_row(agent, cstore):
    agent.dispatcher.recovered = True
    cstore.add(make_row(1, "save", status="running", started_at=old(600)))
    cstore.fail["claim"] = 1
    redrive(cstore, agent.dispatcher)
    assert cstore.rows[1].status == "pending" and cstore.rows[1].message == STALE_MESSAGE


def test_a_live_job_is_never_requeued(agent, cstore, runner):
    agent.dispatcher.recovered = True
    gate = threading.Event()

    def slow(argv):
        gate.wait(5)
        return runner.engine_records("done")(argv)
    runner.behave = slow
    cstore.add(make_row(5, "save"))
    agent.dispatcher.dispatch(cstore.rows[5])
    # Even if it looks stale (an old started_at), a row this process owns stays put.
    cstore.rows[5] = dataclasses.replace(cstore.rows[5], started_at=old(600))
    redrive(cstore, agent.dispatcher)
    again = agent.dispatcher.dispatch(cstore.rows[5])
    assert "already running" in again.message
    gate.set()
    agent.wait()
    assert cstore.requeues == [] and len(runner.calls) == 1


def test_db_error_after_the_claim_then_rpc_retry_redrives(stub, agent, cstore, runner):
    """R#1: the claim succeeded, then the database failed: the row is 'running' with nobody on
    it. The retry of the same audit_id re-drives it instead of answering 'already running'."""
    agent.dispatcher.recovered = True
    cstore.add(make_row(10, "save"))
    cstore.fail["published_at"] = 1
    with pytest.raises(grpc.RpcError) as err:
        run_desk(stub, 10)
    assert err.value.code() == grpc.StatusCode.UNAVAILABLE
    assert cstore.rows[10].status == "running" and runner.calls == []
    reply = run_desk(stub, 10)
    assert reply.status == pb.COMMAND_STATUS_ACCEPTED and "queued" in reply.message
    agent.wait()
    assert cstore.requeues == [10] and cstore.claims == [10, 10]
    assert cstore.rows[10].status == "done" and len(runner.calls) == 1


def test_outcome_write_fails_then_the_periodic_task_redrives(agent, cstore, runner):
    """R#2: the engine left the row 'running' and the agent could not mark it failed (DB
    down): the row is abandoned, and the next periodic pass requeues it at once."""
    agent.dispatcher.recovered = True
    runner.behave = lambda argv: RunResult(rc=139, stderr="Segmentation fault\n")
    cstore.add(make_row(20, "save"))
    cstore.fail["get_row"] = 1                # _fail_if_running cannot read the row
    agent.dispatcher.dispatch(cstore.rows[20])
    agent.wait()
    assert cstore.rows[20].status == "running"
    assert 20 not in agent.dispatcher.live_ids()
    runner.behave = runner.engine_records("done")
    redrive(cstore, agent.dispatcher)
    agent.wait()
    assert cstore.requeues == [20] and cstore.rows[20].status == "done"
    assert len(runner.calls) == 2


def test_young_unowned_running_row_is_left_alone(stub, agent, cstore, runner):
    """A row an engine started by hand (not this process) gets job_timeout + grace."""
    agent.dispatcher.recovered = True
    cstore.add(make_row(30, "save", status="running", started_at=old(1)))
    reply = run_desk(stub, 30)
    assert "already running" in reply.message
    assert cstore.requeues == [] and runner.calls == []


def test_terminal_rows_are_final(stub, agent, cstore, runner):
    for i, status in enumerate(("done", "refused", "failed"), start=40):
        cstore.add(make_row(i, "save", status=status, message="earlier"))
        reply = run_desk(stub, i)
        assert f"already {status}" in reply.message
    redrive(cstore, agent.dispatcher)
    agent.wait()
    assert runner.calls == [] and cstore.claims == []


# -- 2. decisions (C2, C3) ------------------------------------------------------------------

def requested(cstore, id=50, **kw):
    kw.setdefault("status", "done")
    return cstore.add(make_row(id, "override_request", token_hash="ab" * 32,
                               token_expires_at=now() + dt.timedelta(hours=40), **kw))


def decision(cstore, id, parent=50, approved=True, **kw):
    return cstore.add(make_row(id, "override_decision", parent_id=parent, approver_role="vp",
                               requested_by="vp@algogators.com", payload={"approved": approved},
                               reason=None, **kw))


def test_second_decision_after_a_done_one_is_refused(stub, agent, cstore, runner):
    requested(cstore)
    decision(cstore, 51)
    decide(stub, 51)
    agent.wait()
    assert cstore.rows[51].status == "done"
    decision(cstore, 52, approved=False)
    reply = decide(stub, 52)
    assert reply.status == pb.COMMAND_STATUS_REFUSED
    assert reply.message == "override request 50 was already decided (decision 51)"
    agent.wait()
    assert len(runner.calls) == 1


def test_a_rejection_also_closes_the_request(stub, agent, cstore, runner):
    requested(cstore)
    decision(cstore, 51, approved=False)
    assert decide(stub, 51).status == pb.COMMAND_STATUS_DONE
    decision(cstore, 52)
    assert decide(stub, 52).status == pb.COMMAND_STATUS_REFUSED
    agent.wait()
    assert runner.calls == []


def test_a_refused_decision_does_not_block_the_next(stub, agent, cstore, runner):
    requested(cstore)
    decision(cstore, 51, status="refused", message="earlier")
    decision(cstore, 52)
    assert decide(stub, 52).status == pb.COMMAND_STATUS_ACCEPTED
    agent.wait()
    assert cstore.rows[52].status == "done"


def test_concurrent_decisions_run_the_engine_once(agent, cstore, runner):
    """Two approvers' rows dispatched at the same moment: one runs, the other is refused."""
    requested(cstore)
    decision(cstore, 51)
    decision(cstore, 52)
    gate = threading.Event()

    def slow(argv):
        gate.wait(5)
        return runner.engine_records("done")(argv)
    runner.behave = slow
    barrier = threading.Barrier(2)
    outs = {}

    def go(i):
        barrier.wait()
        outs[i] = agent.dispatcher.dispatch(cstore.rows[i])
    threads = [threading.Thread(target=go, args=(i,)) for i in (51, 52)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    gate.set()
    agent.wait()
    statuses = sorted(o.status for o in outs.values())
    assert statuses == ["accepted", "refused"]
    assert len(runner.calls) == 1
    refused = next(i for i, o in outs.items() if o.status == "refused")
    assert "is being decided by decision" in cstore.rows[refused].message


@pytest.mark.parametrize("parent_status", ["pending", "running", "failed"])
def test_decision_on_a_request_that_is_not_done(stub, agent, cstore, runner, parent_status):
    requested(cstore, status=parent_status)
    decision(cstore, 51)
    reply = decide(stub, 51)
    assert reply.status == pb.COMMAND_STATUS_REFUSED
    assert f"override request 50 is {parent_status}, not done" == reply.message
    assert runner.calls == []


def test_decision_on_a_published_day_is_refused(stub, agent, cstore, runner):
    requested(cstore)
    decision(cstore, 51)
    cstore.published[(PID, DAY)] = now()
    reply = decide(stub, 51)
    assert reply.status == pb.COMMAND_STATUS_REFUSED and "frozen" in reply.message
    agent.wait()
    assert runner.calls == []


@pytest.mark.parametrize("kind", ["save", "override_request"])
def test_published_day_refuses_save_and_override_request(stub, agent, cstore, runner, kind):
    cstore.published[(PID, DAY)] = now()
    cstore.add(make_row(60, kind))
    out = agent.dispatcher.dispatch(cstore.rows[60])
    agent.wait()
    assert out.status == "refused" and "a published day is frozen" in out.message
    assert cstore.rows[60].status == "refused" and runner.calls == [] and agent.sent == []


def test_an_approval_of_a_published_day_is_refused_unless_it_approved_it(agent, cstore, runner):
    """Contract C7: once published, an approval is refused (the engine refuses too); a re-run of
    the row that approved the day (its result carries published_at) goes on to its send."""
    cstore.published[(PID, DAY)] = now()
    cstore.add(make_row(61, "publish"))
    out = agent.dispatcher.dispatch(cstore.rows[61])
    assert out.status == "refused" and "already published" in out.message
    cstore.add(make_row(62, "publish", result={"published_at": "x", "publish_source": "desk"}))
    agent.now = DISPATCH_AT.replace(hour=9, minute=40)
    assert agent.dispatcher.dispatch(cstore.rows[62]).status == "accepted"
    agent.wait()
    assert [c[0][3:] for c in runner.calls][0][0] == "--publish"
    assert runner.calls[0][0][-1] == "--send-now"


# -- 3. override request: re-drive and the e-mail-disabled rule (C1, R#4, R#6) --------------

GOOD_SMTP = {"smtp_host": "smtp.example.com", "smtp_port": 587, "username": "bot@example.com",
             "password": "app-password-123", "from_email": "qt@example.com", "use_tls": True}


def mail_on(agent, settings, **changes):
    agent.dispatcher._settings = dataclasses.replace(settings, email_disabled=False, **changes)


def test_redrive_after_the_mail_never_sends_again(agent, cstore, settings, config_dir):
    write_portfolio(config_dir, email=GOOD_SMTP)
    mail_on(agent, settings)
    stored = {"emailed": ["vp", "president"], "emailed_at": "2026-10-07T14:00:00+00:00"}
    cstore.add(make_row(70, "override_request", status="running", started_at=old(1),
                        token_hash="cd" * 32, token_expires_at=now() + dt.timedelta(hours=47),
                        result=stored))
    redrive(cstore, agent.dispatcher)         # startup: requeue + dispatch
    agent.wait()
    row = cstore.rows[70]
    assert row.status == "done" and row.message == "already e-mailed; not sent again"
    assert row.token_hash == "cd" * 32 and row.result == stored
    assert agent.sent == []


def test_mail_is_sent_then_token_and_emailed_at_stored_then_done(agent, cstore, settings,
                                                                config_dir):
    write_portfolio(config_dir, email=GOOD_SMTP)
    mail_on(agent, settings)
    order = []
    real_record = cstore.record_mail

    def record(*a):
        order.append(("record", cstore.rows[70].status))
        return real_record(*a)
    cstore.record_mail = record
    agent.dispatcher._sender = lambda cfg, to, subject, body: order.append(("send", body))
    cstore.add(make_row(70, "override_request"))
    agent.dispatcher.dispatch(cstore.rows[70])
    agent.wait()
    assert [o[0] for o in order] == ["send", "record"] and order[1][1] == "running"
    token = re.search(r"token=([A-Za-z0-9_-]+)", order[0][1]).group(1)
    row = cstore.rows[70]
    assert row.token_hash == hashlib.sha256(token.encode()).hexdigest()
    assert row.status == "done" and "emailed_at" in row.result


def test_token_not_stored_after_the_send_fails_the_row(agent, cstore, settings, config_dir):
    write_portfolio(config_dir, email=GOOD_SMTP)
    mail_on(agent, settings)
    cstore.fail["record_mail"] = 1
    cstore.add(make_row(70, "override_request"))
    agent.dispatcher.dispatch(cstore.rows[70])
    agent.wait()
    row = cstore.rows[70]
    assert len(agent.sent) == 1
    assert row.status == "failed" and row.token_hash is None


@pytest.mark.parametrize("email,why", [
    (None, "no "),
    ({**GOOD_SMTP, "enabled": False}, "enabled=false"),
    ({**GOOD_SMTP, "password": "YOUR_SMTP_APP_PASSWORD"}, "no SMTP credentials"),
])
def test_email_misconfiguration_fails_without_a_token(agent, cstore, settings, config_dir,
                                                      email, why):
    """C1: only QT_EMAIL_DISABLED=1 may put the link in the result."""
    (config_dir / "portfolios" / "qt_conservative" / "email.json").unlink(missing_ok=True)
    if email is not None:
        write_portfolio(config_dir, email=email)
    mail_on(agent, settings)
    cstore.add(make_row(70, "override_request"))
    agent.dispatcher.dispatch(cstore.rows[70])
    agent.wait()
    row = cstore.rows[70]
    assert row.status == "failed" and why in row.message and "QT_EMAIL_DISABLED=1" in row.message
    assert row.token_hash is None and row.result is None and agent.sent == []


def test_email_disabled_explicitly_keeps_the_link_in_the_result(agent, cstore):
    cstore.add(make_row(70, "override_request"))
    agent.dispatcher.dispatch(cstore.rows[70])
    agent.wait()
    row = cstore.rows[70]
    assert row.status == "done" and row.result["email_disabled"] is True
    token = re.search(r"token=([A-Za-z0-9_-]+)", row.result["email"]["body"]).group(1)
    assert row.token_hash == hashlib.sha256(token.encode()).hexdigest()


# -- 4. runtime: bounded output, timeouts, health ---------------------------------------------

posix = pytest.mark.skipif(os.name != "posix" or shutil.which("sh") is None,
                           reason="needs a POSIX shell")


@posix
def test_engine_output_is_bounded(tmp_path):
    script = tmp_path / "chatty.sh"
    script.write_text("#!/bin/sh\ni=0\nwhile [ $i -lt 60000 ]; do echo \"line $i of chatter "
                      "padding padding padding\"; i=$((i+1)); done\necho last-err >&2\nexit 4\n")
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    res = SubprocessRunner().run([str(script)], str(tmp_path), 30)
    assert res.rc == 4 and len(res.stdout) <= TAIL_BYTES
    assert res.stdout.splitlines()[-1] == "line 59999 of chatter padding padding padding"
    assert res.stdout.splitlines()[0].startswith("line ")     # no partial first line
    assert res.stderr.strip() == "last-err"


@posix
def test_engine_output_copy_and_timeout(tmp_path):
    import io
    script = tmp_path / "slow.sh"
    script.write_text("#!/bin/sh\necho started\nsleep 30\n")
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    copy = io.StringIO()
    t0 = time.monotonic()
    res = SubprocessRunner().run([str(script)], str(tmp_path), 0.5, copy_to=copy)
    assert res.timed_out and time.monotonic() - t0 < 10
    assert "started" in copy.getvalue() and "started" in res.stdout


def test_health_port_answers_while_the_main_pool_is_busy(agent):
    """R#9: every main worker stuck in a slow call; the health-only port still answers."""
    release = threading.Event()

    class SlowStore(FakeStore):
        def run_facts(self, portfolio_id, date):
            release.wait(10)
            return RunFacts()

    reg = Registry()
    reg.register(build_service(SlowStore(), agent.dispatcher))
    server, health_servicer, port = build_server(reg, "127.0.0.1:0", max_workers=1)
    hserver, hport = build_health_server(health_servicer, "127.0.0.1:0")
    server.start()
    hserver.start()
    try:
        ch = versioned_channel(f"127.0.0.1:{port}")
        stub = desk_pb2_grpc.DeskServiceStub(ch)
        pending = stub.GetRunStatus.future(pb.RunStatusRequest(portfolio_id=PID, date=DATE),
                                           timeout=15)
        time.sleep(0.2)                       # the only main worker is now blocked
        with grpc.insecure_channel(f"127.0.0.1:{hport}") as hch:
            t0 = time.monotonic()
            reply = health_pb2_grpc.HealthStub(hch).Check(health_pb2.HealthCheckRequest(),
                                                          timeout=3)
            assert reply.status == health_pb2.HealthCheckResponse.SERVING
            assert time.monotonic() - t0 < 2
        release.set()
        pending.result()
        ch.close()
    finally:
        release.set()
        server.stop(grace=None)
        hserver.stop(grace=None)


def test_connections_have_timeouts():
    kw = DbConfig("h", "5432", "u", "n", "pw").conninfo_kwargs()
    assert kw["connect_timeout"] == CONNECT_TIMEOUT_S == 5
    assert kw["options"] == "-c statement_timeout=30000"


def test_catchup_settings():
    s = catchup_settings({})
    assert s.enabled and s.portfolios == ("qt_conservative", "qt_conservative_model")
    assert (s.window_start, s.window_end, s.every_minutes) == (dt.time(6, 30), dt.time(22), 30)
    assert (s.busy_start, s.busy_end, s.busy_every_minutes) == (dt.time(6, 30), dt.time(10, 30), 5)
    assert s.today_not_before == dt.time(6, 45) and s.model_retry_minutes == 15
    assert s.model_alert_at == dt.time(8, 30)
    s = catchup_settings({"QT_CATCHUP_PORTFOLIOS": "a, b", "QT_CATCHUP_WINDOW": "07:30-20:00",
                          "QT_CATCHUP_EVERY_MIN": "15", "QT_CATCHUP_TODAY_NOT_BEFORE": "11:00"})
    assert s.portfolios == ("a", "b") and s.window_start == dt.time(7, 30)
    assert s.every_minutes == 15 and s.today_not_before == dt.time(11)
    assert catchup_settings({"QT_CATCHUP_ENABLED": "0", "QT_CATCHUP_PORTFOLIOS": ""}).enabled \
        is False
    for bad in ({"QT_CATCHUP_WINDOW": "22:00-06:00"}, {"QT_CATCHUP_EVERY_MIN": "45"},
                {"QT_CATCHUP_TODAY_NOT_BEFORE": "noon"}, {"QT_CATCHUP_PORTFOLIOS": " , "},
                {"QT_CATCHUP_BUSY_WINDOW": "10:30-06:30"}, {"QT_CATCHUP_BUSY_EVERY_MIN": "7"}):
        with pytest.raises(ConfigError):
            catchup_settings(bad)


def test_registered_service_has_both_background_tasks(store, agent, cstore, settings,
                                                      tmp_path):
    from algogators_rpc.services.desk.catchup import CatchupScheduler

    sched = CatchupScheduler(None, settings, catchup_settings({}))
    service = build_service(store, agent.dispatcher, cstore, 60, sched)
    assert [t.name for t in service.background_tasks] == ["desk.redrive", "desk.catchup"]
    assert service.background_tasks[1].interval_s == 60
