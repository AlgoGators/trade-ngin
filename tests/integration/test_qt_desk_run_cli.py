"""Explicit accounting CLI; PG cases use the existing owned route-less fixture."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

import pytest
from test_qt_desk_accounting import accounting, desk, connection
from test_qt_desk_upstream import upstream

BINARY = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/qt_desk_run")
DECISION = "40000000-0000-4000-8000-000000000001"
ATTEMPT = "50000000-0000-4000-8000-000000000001"
INPUT = "90000000-0000-4000-8000-000000000001"
MARKET = "a0000000-0000-4000-8000-000000000001"
FINAL = "qt-finalization/f0000000-0000-4000-8000-000000000001"
SOURCED_INPUT = "d0000000-0000-4000-8000-000000000001"


def invoke(args, *, fd=None, environment=None):
    assert BINARY.is_file(), "Supported qt_desk_run accounting executable is missing"
    command = [str(BINARY), *args]
    if fd is not None:
        command += ["--connection-fd", str(fd)]
    if environment is None:
        environment = dict(PATH="/usr/local/bin:/usr/bin:/bin",
                           LD_PRELOAD="/home/devcontainers/qt-validation-20260921/bin/Debug/libqt_no_delivery_guard.so")
    return subprocess.run(command, capture_output=True, text=True, timeout=15,
                          pass_fds=() if fd is None else (fd,), env=environment)


def args(day="2026-09-26", *, decision=DECISION, attempt=ATTEMPT, input_id=INPUT,
         market_source=None, finalization_source=None):
    result = ["--desk", day, "--decision", decision,
              "--attempt", attempt, "--input", input_id]
    if market_source is not None:
        result += ["--market-source", market_source]
    if finalization_source is not None:
        result += ["--finalization-source", finalization_source]
    return result


def response(result, code, reason):
    assert result.returncode == code, result.stdout + result.stderr
    assert result.stderr == ""
    payload = json.loads(result.stdout)
    assert payload["status"] == reason
    return payload


def test_help_is_available_without_a_database():
    result = invoke(["--help"])
    assert result.returncode == 0
    assert "--desk YYYY-MM-DD" in result.stdout
    assert "--connection-fd N" in result.stdout
    assert "--market-source UUID" in result.stdout
    assert "--finalization-source qt-finalization/UUID" in result.stdout


@pytest.mark.parametrize("arguments", [
    [], args(), args() + ["--connection-fd", "0"],
    args() + ["--connection-fd", "03"], args() + ["--connection-fd", "-1"],
    args() + ["--connection-fd", "2147483648"],
    args() + ["--decision", DECISION, "--connection-fd", "999"],
    args() + ["--unknown", "secret-never-echo", "--connection-fd", "999"],
    args("2026-02-30") + ["--connection-fd", "999"],
    args("2026-9-26") + ["--connection-fd", "999"],
    args()[:-1] + ["NOT-A-UUID", "--connection-fd", "999"],
    args() + ["--connection-fd"],
])
def test_invalid_arguments_fail_before_connection_access(arguments):
    result = invoke(arguments)
    response(result, 2, "invalid_arguments")
    assert "secret-never-echo" not in result.stdout


@pytest.mark.parametrize("contents", [b"", b"x" * 4097, b"password=never-echo",
    b"host=/tmp/socket dbname=test user=test password=never-echo service=forbidden",
    b"host=127.0.0.1 dbname=test user=test password=never-echo",
    b"host=/tmp/socket dbname=test user=test password=never-echo\x00",
])
def test_invalid_connection_material_is_bounded_and_redacted(contents):
    with tempfile.TemporaryFile() as material:
        material.write(contents)
        material.flush()
        fd = os.open(f"/proc/self/fd/{material.fileno()}", os.O_RDONLY)
        try:
            result = invoke(args(), fd=fd)
        finally:
            os.close(fd)
    response(result, 3, "connection_input_unavailable")
    assert "never-echo" not in result.stdout + result.stderr


def test_writable_and_pipe_descriptors_are_rejected_without_waiting():
    with tempfile.TemporaryFile() as writable:
        response(invoke(args(), fd=writable.fileno()), 3, "connection_input_unavailable")
    reader, writer = os.pipe()
    try:
        response(invoke(args(), fd=reader), 3, "connection_input_unavailable")
    finally:
        os.close(reader)
        os.close(writer)


def test_connection_exception_never_echoes_credentials():
    with tempfile.TemporaryFile() as material:
        material.write(b"host=/tmp/qt-desk-run-nonexistent-socket dbname=unused user=unused password=secret-never-echo")
        material.flush()
        fd = os.open(f"/proc/self/fd/{material.fileno()}", os.O_RDONLY)
        try:
            result = invoke(args(), fd=fd)
        finally:
            os.close(fd)
    response(result, 6, "accounting_unavailable")
    assert "secret-never-echo" not in result.stdout + result.stderr


def run_owned(day, **identities):
    # The fixture validates this env connection before creating synthetic schema;
    # only tests read it. The production entry receives its material solely by fd.
    dsn = os.environ["ALGOLENS_TEST_DB"]
    assert dsn.startswith("host=/tmp/algolens-repair-pg-") and "dbname=algolens_test_" in dsn
    with tempfile.TemporaryFile() as material:
        material.write(dsn.encode())
        material.flush()
        fd = os.open(f"/proc/self/fd/{material.fileno()}", os.O_RDONLY)
        try:
            # Defaults must neither redirect the connection nor supply credentials.
            environment = dict(PATH="/usr/local/bin:/usr/bin:/bin", PGHOST="/unavailable", PGDATABASE="unavailable",
                               PGSERVICE="unavailable", PGPASSWORD="secret-never-echo",
                               PGPASSFILE="/unavailable", HOME="/unavailable",
                               LD_PRELOAD="/home/devcontainers/qt-validation-20260921/bin/Debug/libqt_no_delivery_guard.so")
            return invoke(args(day, **identities), fd=fd, environment=environment)
        finally:
            os.close(fd)


@pytest.fixture()
def cli_accounting(accounting):
    return accounting


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_requested_day_cannot_process_another_immutable_day(cli_accounting):
    from test_qt_desk_accounting import all_state
    conn, _ = cli_accounting
    before = all_state(conn)
    response(run_owned("2000-01-01"), 4, "decision_day_unavailable")
    assert all_state(conn) == before


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_real_accounting_receipt_and_idempotent_replay(cli_accounting):
    from test_qt_desk_accounting import all_state
    conn, payload = cli_accounting
    first = response(run_owned(payload["source_day"]), 0, "accounted")
    assert first["decision_id"] == DECISION and first["attempt_id"] == ATTEMPT
    assert first["source_day"] == payload["source_day"] and first["replayed"] is False
    assert len(first["book_digest"]) == 64
    assert "publication_payload" not in first and "investor_published" not in first
    with conn.cursor() as cur:
        for table in ("executions", "live_results", "equity_curve", "qt_desk_receipts", "desk_run_results"):
            cur.execute("SELECT count(*) FROM trading." + table)
            assert cur.fetchone()[0] > 0
    after = all_state(conn)
    replay = response(run_owned(payload["source_day"]), 0, "accounted")
    assert replay == dict(first, replayed=True)
    assert all_state(conn) == after


@pytest.mark.parametrize("accounting", ["missing_price", "missing_finalization"], indirect=True)
@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_real_processor_refuses_missing_evidence(cli_accounting):
    from test_qt_desk_accounting import all_state
    conn, payload = cli_accounting
    before = all_state(conn)
    response(run_owned(payload["source_day"]), 5, "accounting_refused")
    assert all_state(conn) == before


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_replay_rechecks_current_submitter_grant(cli_accounting):
    from test_qt_desk_accounting import all_state
    conn, payload = cli_accounting
    response(run_owned(payload["source_day"]), 0, "accounted")
    with conn.cursor() as cur:
        cur.execute("UPDATE trading.qt_action_grants SET active=false,version=2 "
                    "WHERE user_id=1 AND capability='qt_submit'")
    before = all_state(conn)
    response(run_owned(payload["source_day"]), 5, "accounting_refused")
    assert all_state(conn) == before


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
@pytest.mark.parametrize("identity", ["attempt", "input_id"])
def test_replay_refuses_a_different_attempt_or_input(cli_accounting, identity):
    from test_qt_desk_accounting import all_state
    conn, payload = cli_accounting
    response(run_owned(payload["source_day"]), 0, "accounted")
    before = all_state(conn)
    response(run_owned(payload["source_day"], **{identity: "a0000000-0000-4000-8000-000000000001"}),
             5, "accounting_refused")
    assert all_state(conn) == before


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_pending_immutable_decision_is_not_a_confirmation(cli_accounting):
    from test_qt_desk_accounting import all_state
    conn, payload = cli_accounting
    pending = "a0000000-0000-4000-8000-000000000001"
    preview = "b0000000-0000-4000-8000-000000000001"
    with conn.cursor() as cur:
        cur.execute("INSERT INTO trading.qt_previews SELECT (jsonb_populate_record("
                    "NULL::trading.qt_previews,to_jsonb(p)||jsonb_build_object("
                    "'preview_id',%s::text,'state','pending_override'))).* "
                    "FROM trading.qt_previews p", (preview,))
        cur.execute("INSERT INTO trading.qt_decisions SELECT (jsonb_populate_record("
                    "NULL::trading.qt_decisions,to_jsonb(d)||jsonb_build_object("
                    "'decision_id',%s::text,'preview_id',%s::text,'status','pending_override'))).* "
                    "FROM trading.qt_decisions d WHERE decision_id=%s", (pending, preview, DECISION))
    before = all_state(conn)
    response(run_owned(payload["source_day"], decision=pending), 4, "decision_day_unavailable")
    assert all_state(conn) == before


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
@pytest.mark.parametrize("unavailable", ["input", "policy"])
def test_absent_accounting_source_or_disabled_policy_is_refused(cli_accounting, unavailable):
    from test_qt_desk_accounting import all_state
    conn, payload = cli_accounting
    identities = {}
    if unavailable == "input":
        identities["input_id"] = "a0000000-0000-4000-8000-000000000001"
    else:
        with conn.cursor() as cur:
            cur.execute("UPDATE trading.qt_source_policies SET enabled=false,version=2 WHERE purpose='execution'")
    before = all_state(conn)
    response(run_owned(payload["source_day"], **identities), 5, "accounting_refused")
    assert all_state(conn) == before


@pytest.mark.parametrize("source_first", [False, True])
def test_valid_source_pair_reaches_fd_admission_before_database(source_first):
    arguments = args(market_source=MARKET, finalization_source=FINAL)
    if source_first:
        arguments = arguments[-4:] + arguments[:-4]
    response(invoke(arguments + ["--connection-fd", "999"]),
             3, "connection_input_unavailable")


@pytest.mark.parametrize("arguments", [
    args(market_source=MARKET), args(finalization_source=FINAL),
    args(market_source=MARKET.upper(), finalization_source=FINAL),
    args(market_source="not-a-uuid", finalization_source=FINAL),
    args(market_source=MARKET, finalization_source=FINAL.removeprefix("qt-finalization/")),
    args(market_source=MARKET, finalization_source=FINAL.upper()),
    args(market_source=MARKET, finalization_source=FINAL + "/extra"),
    args(market_source=MARKET, finalization_source="qt-finalization/not-a-uuid"),
    args(market_source=MARKET, finalization_source="opaque-secret-never-echo"),
    args(market_source=MARKET, finalization_source=FINAL)[:6]
        + args(market_source=MARKET, finalization_source=FINAL)[8:],
    args(market_source=MARKET, finalization_source=FINAL) + ["--market-source", MARKET],
    args(market_source=MARKET) + ["--unknown", "secret-never-echo"],
])
def test_source_pair_and_explicit_input_grammar_fail_before_fd_access(arguments):
    result = invoke(arguments + ["--connection-fd", "999"])
    response(result, 2, "invalid_arguments")
    assert "secret-never-echo" not in result.stdout + result.stderr


@pytest.fixture()
def cli_upstream(upstream):
    # Only the explicit existing source/finalizer probes prepare this synthetic
    # fixture. The actual CLI neither discovers nor captures upstream sources.
    from test_qt_desk_upstream import invoke as source_probe, OLD, FINAL as FINAL_UUID
    conn, evidence, _ = upstream
    captured = source_probe("--market", payload=evidence)
    assert captured.returncode == 0, captured.stdout + captured.stderr
    finalized = source_probe("--finalize", OLD, FINAL_UUID, MARKET)
    assert finalized.returncode == 0, finalized.stdout + finalized.stderr
    return conn, evidence["payload"]["source_day"]


def sourced_owned(day, **identities):
    supplied = dict(input_id=SOURCED_INPUT, market_source=MARKET, finalization_source=FINAL)
    supplied.update(identities)
    return run_owned(day, **supplied)


@pytest.mark.parametrize("accounting", ["no_input"], indirect=True)
@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_actual_cli_assembles_explicit_input_and_accounts_sourced_day(cli_upstream):
    from test_qt_desk_upstream import upstream_state, FINAL as FINAL_UUID
    conn, day = cli_upstream
    with conn.cursor() as cur:
        cur.execute("SELECT count(*) FROM trading.qt_desk_accounting_inputs WHERE input_id=%s",
                    (SOURCED_INPUT,))
        assert cur.fetchone()[0] == 0
    first = response(sourced_owned(day), 0, "accounted")
    assert first["decision_id"] == DECISION and first["attempt_id"] == ATTEMPT
    assert first["source_day"] == day and first["replayed"] is False
    assert len(first["book_digest"]) == 64
    assert set(first) == {"schema", "status", "decision_id", "attempt_id",
                          "source_day", "book_digest", "replayed"}
    with conn.cursor() as cur:
        cur.execute("SELECT payload FROM trading.qt_desk_accounting_inputs WHERE input_id=%s",
                    (SOURCED_INPUT,))
        assembled = cur.fetchone()[0]
        assert assembled["schema_version"] == "qt-futures-accounting-input/v2"
        assert assembled["market_source_id"] == MARKET
        assert assembled["prior_finalization_source_id"] == FINAL
        cur.execute("SELECT payload FROM trading.qt_desk_finalizations WHERE finalization_id=%s",
                    (FINAL_UUID,))
        transition = cur.fetchone()[0]
        assert assembled["previous_totals"][0]["equity_exact"] == transition["engine_totals"][0]["equity_exact"]
        cur.execute("SELECT payload FROM trading.desk_run_results WHERE decision_id=%s", (DECISION,))
        output = cur.fetchone()[0]
        assert output["executions"] == []  # Real selected quiet carry.
        assert output["observation"]["schema_version"] == "qt-execution/v2"
    after = upstream_state(conn)
    assert response(sourced_owned(day), 0, "accounted") == dict(first, replayed=True)
    assert upstream_state(conn) == after


@pytest.mark.parametrize("accounting", ["no_input"], indirect=True)
@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_sourced_cli_day_binding_precedes_input_assembly(cli_upstream):
    from test_qt_desk_upstream import upstream_state
    conn, _ = cli_upstream
    before = upstream_state(conn)
    response(sourced_owned("2000-01-01"), 4, "decision_day_unavailable")
    assert upstream_state(conn) == before


@pytest.mark.parametrize("accounting", ["no_input"], indirect=True)
@pytest.mark.parametrize("desk", ["futures"], indirect=True)
@pytest.mark.parametrize("missing", ["market_source", "finalization_source"])
def test_missing_sourced_evidence_refuses_without_assembled_input(cli_upstream, missing):
    from test_qt_desk_upstream import upstream_state
    conn, day = cli_upstream
    absent = "e0000000-0000-4000-8000-000000000001"
    value = absent if missing == "market_source" else "qt-finalization/" + absent
    before = upstream_state(conn)
    response(sourced_owned(day, **{missing: value}), 5, "accounting_refused")
    assert upstream_state(conn) == before


@pytest.mark.parametrize("accounting", ["no_input"], indirect=True)
@pytest.mark.parametrize("desk", ["futures"], indirect=True)
@pytest.mark.parametrize("authority", ["grant", "policy"])
def test_sourced_replay_rechecks_current_authority_without_any_write(cli_upstream, authority):
    from test_qt_desk_upstream import upstream_state
    conn, day = cli_upstream
    response(sourced_owned(day), 0, "accounted")
    with conn.cursor() as cur:
        if authority == "grant":
            cur.execute("UPDATE trading.qt_action_grants SET active=false,version=version+1 "
                        "WHERE user_id=1 AND capability='qt_submit'")
        else:
            cur.execute("UPDATE trading.qt_source_policies SET enabled=false,version=version+1 "
                        "WHERE book_id='BOOK' AND purpose='execution'")
    before = upstream_state(conn)
    response(sourced_owned(day), 5, "accounting_refused")
    assert upstream_state(conn) == before


@pytest.mark.parametrize("accounting", ["no_input"], indirect=True)
@pytest.mark.parametrize("desk", ["futures"], indirect=True)
@pytest.mark.parametrize("identity", ["attempt", "input_id"])
def test_sourced_replay_refuses_another_attempt_or_explicit_input(cli_upstream, identity):
    from test_qt_desk_upstream import upstream_state
    conn, day = cli_upstream
    response(sourced_owned(day), 0, "accounted")
    before = upstream_state(conn)
    response(sourced_owned(day, **{identity: "e0000000-0000-4000-8000-000000000001"}),
             5, "accounting_refused")
    assert upstream_state(conn) == before


@pytest.mark.parametrize("accounting", ["no_input"], indirect=True)
@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_sourced_cli_receipt_failure_rolls_back_input_and_financial_rows(cli_upstream):
    from test_qt_desk_upstream import upstream_state
    conn, day = cli_upstream
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.reject_test_cli_receipt() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN RAISE EXCEPTION 'owned CLI receipt failure'; END $$;
          CREATE TRIGGER reject_test_cli_receipt BEFORE INSERT ON trading.qt_desk_receipts
          FOR EACH ROW EXECUTE FUNCTION trading.reject_test_cli_receipt()""")
    before = upstream_state(conn)
    response(sourced_owned(day), 5, "accounting_refused")
    assert upstream_state(conn) == before
