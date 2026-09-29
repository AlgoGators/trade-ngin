"""Migration 013 against the owned, network-none PostgreSQL fixture only."""
import os
import subprocess
import json
import hashlib
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import date
from pathlib import Path

import psycopg2
import pytest


@pytest.fixture()
def connection():
    dsn = os.environ.get("ALGOLENS_TEST_DB", "")
    assert "algolens_test_" in dsn and "host=/tmp/algolens-repair-pg-" in dsn
    conn = psycopg2.connect(dsn)
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute("DROP SCHEMA IF EXISTS trading CASCADE; CREATE SCHEMA trading")
        cur.execute("""
            CREATE TABLE trading.strategy_registry (
              id text PRIMARY KEY, strategy_type text NOT NULL, portfolio_id text NOT NULL,
              lifecycle text NOT NULL DEFAULT 'live', is_active boolean NOT NULL DEFAULT true,
              name text DEFAULT 'Synthetic', description text, initial_equity numeric,
              managers text[],sort_order int DEFAULT 0,mock_capital numeric);
            CREATE TABLE trading.strategy_book_memberships (
              strategy_id text REFERENCES trading.strategy_registry(id), portfolio_id text,
              PRIMARY KEY(strategy_id, portfolio_id));
            INSERT INTO trading.strategy_registry (id,strategy_type,portfolio_id)
              VALUES ('trend','LIVE_TREND','BOOK');
            CREATE TABLE trading.positions (strategy_id text, portfolio_id text,
              portfolio_type text, quantity numeric, symbol text, average_price numeric,
              daily_unrealized_pnl numeric, daily_realized_pnl numeric, last_update timestamptz,
              updated_at timestamptz, strategy_name text, date date,
              UNIQUE(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type));
            CREATE TABLE trading.risk_limits (strategy_id text, portfolio_id text, limits jsonb,
              id bigserial,published_at timestamptz DEFAULT now());
            CREATE TABLE trading.live_results (strategy_id text, portfolio_id text,
              portfolio_type text NOT NULL DEFAULT 'system',
              date timestamptz, total_pnl numeric, config jsonb,current_portfolio_value numeric,
              total_annualized_return numeric,volatility numeric,total_cumulative_return numeric);
            CREATE TABLE trading.equity_curve (strategy_id text, portfolio_id text,
              timestamp timestamptz, equity numeric, portfolio_type text,
              UNIQUE(portfolio_id,strategy_id,timestamp,portfolio_type));
            CREATE TABLE trading.executions (strategy_id text, portfolio_id text,
              portfolio_type text NOT NULL DEFAULT 'system');
            CREATE TABLE trading.signals (strategy_id text, portfolio_id text);
            CREATE TABLE trading.live_run_metadata (strategy_id text, portfolio_id text,
              date date, strategy_allocations jsonb,portfolio_config jsonb,strategy_configs jsonb,
              created_at timestamptz NOT NULL DEFAULT now(),
              UNIQUE(date,strategy_id,portfolio_id));
            CREATE TABLE trading.run_inputs (strategy_id text, portfolio_id text,date date,
              trade_ngin_sha text,config_snapshot jsonb,universe jsonb,data_window jsonb,
              risk_limits_id bigint,engine_flags jsonb,UNIQUE(portfolio_id,strategy_id,date));
        """)
        migration = Path(__file__).parents[2] / "migrations/013_runtime_control.sql"
        for name in ('004_position_overrides_audit.sql','012_position_overrides_portfolio_scope.sql'):
            cur.execute((migration.parent / name).read_text())
        if migration.exists():
            cur.execute(migration.read_text())
    yield conn
    conn.close()


def prepare_exact_publication_schema(connection):
    """Add the proposal and exact-seed capabilities for successful final publications."""
    from test_proposal_storage_migration import (
        MIGRATION as PROPOSAL_MIGRATION,
        apply,
        normalize_positions_shape,
    )

    normalize_positions_shape(connection)
    apply(connection, PROPOSAL_MIGRATION)
    apply(connection, Path(__file__).parents[2] /
          "migrations/016_qt_exact_precision_and_seed_provenance.sql")


def test_runtime_schema_tracks_control_generation_and_keeps_snapshot_immutable(connection):
    with connection.cursor() as cur:
        cur.execute("SELECT to_regclass('trading.runtime_intents')")
        assert cur.fetchone()[0] is not None, "runtime intent persistence is missing"
        cur.execute("SELECT runtime_revision FROM trading.strategy_registry WHERE id='trend'")
        assert cur.fetchone()[0] == 0
        cur.execute("UPDATE trading.strategy_registry SET lifecycle='retired' WHERE id='trend'")
        cur.execute("INSERT INTO trading.strategy_book_memberships VALUES ('trend','BOOK2')")
        cur.execute("SELECT runtime_revision FROM trading.strategy_registry WHERE id='trend'")
        assert cur.fetchone()[0] == 2
        cur.execute("""INSERT INTO trading.runtime_intents
            (registry_id, portfolio_id, engine_strategy_id, action, registry_revision,
             config_snapshot, status, requested_by, request_reason)
            VALUES ('trend','BOOK','LIVE_TREND','stop',2,'{"snapshot_version":1}',
                    'pending','1','reviewed stop') RETURNING id""")
        intent = cur.fetchone()[0]
        with pytest.raises(psycopg2.Error):
            cur.execute("UPDATE trading.runtime_intents SET config_snapshot='{}' WHERE id=%s", (intent,))
        with pytest.raises(psycopg2.Error):
            cur.execute("UPDATE trading.runtime_intents SET status='approved' WHERE id=%s", (intent,))
        with pytest.raises(psycopg2.Error):
            cur.execute("""UPDATE trading.runtime_intents SET id=id+100,status='approved',
                approved_by='admin',approval_reason='otherwise valid',approved_at=now()
                WHERE id=%s""",(intent,))
        cur.execute("""UPDATE trading.runtime_intents SET status='approved', approved_by='admin',
            approval_reason='approved explicit stop', approved_at=now() WHERE id=%s""", (intent,))
        with pytest.raises(psycopg2.Error):
            cur.execute("DELETE FROM trading.runtime_intents WHERE id=%s", (intent,))


def test_runtime_attempt_cannot_claim_applied_without_publication(connection):
    with connection.cursor() as cur:
        cur.execute("SELECT to_regclass('trading.runtime_attempts')")
        assert cur.fetchone()[0] is not None, "runtime attempt persistence is missing"
        cur.execute("""INSERT INTO trading.runtime_intents
            (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
             config_snapshot,status,requested_by,request_reason)
            VALUES ('trend','BOOK','LIVE_TREND','run',0,'{"snapshot_version":1}',
                    'pending','1','request') RETURNING id""")
        intent = cur.fetchone()[0]
        cur.execute("""INSERT INTO trading.runtime_attempts
            (id,intent_id,registry_revision,config_snapshot,run_date,producer_version,status)
            VALUES ('attempt-1',%s,0,'{"snapshot_version":1}','2026-09-22','local','running')""", (intent,))
        with pytest.raises(psycopg2.Error):
            cur.execute("UPDATE trading.runtime_attempts SET status='applied' WHERE id='attempt-1'")
        cur.execute("""UPDATE trading.runtime_attempts SET status='failed',
            failure_code='stale_registry',finished_at=now() WHERE id='attempt-1'""")
        with pytest.raises(psycopg2.Error):
            cur.execute("UPDATE trading.runtime_attempts SET status='running' WHERE id='attempt-1'")


def test_generic_operational_writer_cannot_reopen_retired_registry(connection):
    with connection.cursor() as cur:
        cur.execute("UPDATE trading.strategy_registry SET lifecycle='retired' WHERE id='trend'")
        with pytest.raises(psycopg2.Error):
            cur.execute("INSERT INTO trading.positions (strategy_id,portfolio_id,portfolio_type,quantity) "
                        "VALUES ('LIVE_TREND','BOOK','system',12)")
        cur.execute("SELECT count(*) FROM trading.positions")
        assert cur.fetchone()[0] == 0


def test_stream_changes_cannot_hide_operational_rows_and_replay_remains_supported(connection):
    with connection.cursor() as cur:
        cur.execute("INSERT INTO trading.positions (strategy_id,portfolio_id,portfolio_type,quantity) "
                    "VALUES ('LIVE_TREND','BOOK','system',12)")
        with pytest.raises(psycopg2.Error):
            cur.execute("UPDATE trading.positions SET portfolio_type='benchmark'")
        with pytest.raises(psycopg2.Error):
            cur.execute("INSERT INTO trading.positions (strategy_id,portfolio_id,portfolio_type,quantity) "
                        "VALUES ('LIVE_TREND','BOOK','unrecognized',12)")
        cur.execute("UPDATE trading.strategy_registry SET lifecycle='retired'")
        for stream in ('benchmark','benchmark_rebench','benchmark_frozen_shadow'):
            cur.execute("INSERT INTO trading.positions (strategy_id,portfolio_id,portfolio_type,quantity) "
                        "VALUES ('LIVE_TREND','BOOK',%s,12)", (stream,))
        cur.execute("SELECT count(*) FROM trading.positions")
        assert cur.fetchone()[0] == 4


def test_migration_is_repeatable_and_refuses_incomplete_writer_prerequisites(connection):
    migration = Path(__file__).parents[2] / "migrations/013_runtime_control.sql"
    with connection.cursor() as cur:
        cur.execute(migration.read_text())
        cur.execute("DROP TABLE trading.signals")
        with pytest.raises(psycopg2.Error):
            cur.execute(migration.read_text())
        connection.rollback()


@pytest.mark.parametrize("mode", ["publish", "rollback", "retire", "incomplete",
                                 "publish_controlled", "rollback_controlled", "retire_controlled",
                                 "legacy_changed", "membership", "stop_controlled",
                                 "missing_member", "missing_member_stale", "stale_inputs",
                                 "publish_disabled_intent", "historical_valid", "historical_aba",
                                 "historical_aba_controlled", "historical_superseded_controlled"])
def test_cpp_publication_is_atomic_and_captures_values(connection, mode):
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    assert probe.exists(), "runtime publication probe has not been built"
    if mode in ("publish", "publish_controlled", "publish_disabled_intent", "historical_valid"):
        prepare_exact_publication_schema(connection)
    with connection.cursor() as cur:
        if mode.startswith("historical_"):
            cur.execute("""INSERT INTO trading.positions
                (strategy_id,portfolio_id,strategy_name,date,symbol,portfolio_type,quantity,
                 last_update,average_price,daily_unrealized_pnl,daily_realized_pnl)
                VALUES ('LIVE_TREND','BOOK','TREND','2026-09-21','ES','system',5,
                        '2026-09-21',100,0,0)""")
        if mode.startswith("missing_member"):
            cur.execute("INSERT INTO trading.strategy_registry (id,strategy_type,portfolio_id) "
                        "VALUES ('combined','LIVE_MISSING_TREND','BOOK')")
        if mode == "missing_member_stale":
            cur.execute("""INSERT INTO trading.positions
                (strategy_id,portfolio_id,strategy_name,date,symbol,portfolio_type,quantity)
                SELECT 'LIVE_MISSING_TREND','BOOK','MISSING','2026-09-22','ES',stream,7
                FROM unnest(ARRAY['system','qt']) stream""")
        if mode == "stale_inputs":
            exported = subprocess.run([str(probe),"snapshot"],capture_output=True,text=True,check=True)
            snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=",1)[1])
            cur.execute("""INSERT INTO trading.run_inputs
                (strategy_id,portfolio_id,date,trade_ngin_sha,config_snapshot,universe,data_window,engine_flags)
                VALUES ('LIVE_TREND','BOOK','2026-09-22','local-test',%s::jsonb,'["OTHER"]','null','null')""",
                (json.dumps(snapshot),))
        if mode == "legacy_changed":
            cur.execute("UPDATE trading.strategy_registry SET lifecycle='retired';"
                        "UPDATE trading.strategy_registry SET lifecycle='live'")
        if mode == "membership":
            cur.execute("INSERT INTO trading.strategy_book_memberships VALUES ('trend','SECOND')")
        if mode == "stop_controlled":
            cur.execute("UPDATE trading.strategy_registry SET lifecycle='retired'")
    if "controlled" in mode or mode == "publish_disabled_intent":
        snapshot_result = subprocess.run([str(probe), "snapshot"], capture_output=True, text=True, check=True)
        snapshot = json.loads(snapshot_result.stdout.split("RUNTIME_SNAPSHOT=",1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
              (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,
               status,requested_by,request_reason,approved_by,approval_reason,approved_at)
              SELECT 'trend','BOOK','LIVE_TREND',%s,runtime_revision,%s::jsonb,
                      'approved','1','request','2','reviewed',now()
              FROM trading.strategy_registry WHERE id='trend'""",
                        ("stop" if mode == "stop_controlled" else "run", json.dumps(snapshot)))
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, f"probe failed: {result.returncode}: {result.stdout[-1200:]} {result.stderr[-500:]}"
    assert f"RUNTIME_PUBLICATION_OK={mode}" in result.stdout
    if mode == "publish_disabled_intent":
        with connection.cursor() as cur:
            cur.execute("SELECT count(*) FROM trading.runtime_attempts")
            assert cur.fetchone()[0] == 0, "disabled gate must never consume or acknowledge an intent"
    if mode == "stop_controlled":
        with connection.cursor() as cur:
            cur.execute("SELECT status,outcome FROM trading.runtime_attempts")
            assert cur.fetchone() == ("applied","stopped")
            cur.execute("SELECT count(*) FROM trading.positions")
            assert cur.fetchone()[0] == 0


@pytest.mark.parametrize("mode,expected_reason", [
    ("publish_inspection", "none"),
    ("publish_inspection_controlled", "none"),
    ("publish_inspection_unavailable", "projection_invalid"),
    ("publish_inspection_bad_shape", "capture_failed"),
    ("publish_inspection_bad_version", "capture_failed"),
    ("publish_inspection_bad_path", "capture_failed"),
    ("publish_inspection_missing_field", "capture_failed"),
    ("publish_inspection_missing_strategy_field", "capture_failed"),
    ("publish_inspection_bad_selected", "capture_failed"),
    ("publish_inspection_bad_producer_version", "capture_failed"),
    ("publish_inspection_capacity_uint64", "none"),
])
def test_cpp_capture_is_sealed_only_by_final_sql_publication(connection, mode, expected_reason, tmp_path):
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    prepare_exact_publication_schema(connection)
    if mode == "publish_inspection_controlled":
        snapshot_result = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                         text=True, check=True)
        snapshot = json.loads(snapshot_result.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,config_snapshot,
                 status,requested_by,request_reason,approved_by,approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,
                        'approved','1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.stdout, result.stderr)
    raw_child = result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0]
    child = json.loads(raw_child)
    assert set(child) == {"publication_schema_version", "profile", "authority", "stream",
                          "identity", "captured_at", "publication_recorded_at", "status",
                          "reason", "supplied", "selected_trend", "consumption"}
    assert child["publication_schema_version"] == 1
    assert child["profile"] == "live_portfolio_runner_futures"
    assert child["authority"] == "inspection_only"
    assert child["stream"] == "system"
    assert child["captured_at"] == "2026-09-22T12:34:56.123456Z"
    assert child["publication_recorded_at"].endswith("Z")
    assert child["consumption"] == {"status": "not_collected"}
    assert child["reason"] == expected_reason
    assert child["status"] == ("available" if expected_reason == "none" else "unavailable")
    assert set(child["identity"]) == {"registry_id", "registry_revision", "engine_strategy_id",
                                      "portfolio_id", "run_date", "capture_id", "publication_id",
                                      "runtime_attempt_id", "producer_version", "control_mode"}
    assert child["identity"]["registry_id"] == "trend"
    assert child["identity"]["registry_revision"] == 0
    assert child["identity"]["engine_strategy_id"] == "LIVE_TREND"
    assert child["identity"]["portfolio_id"] == "BOOK"
    assert child["identity"]["run_date"] == "2026-09-22"
    assert child["identity"]["capture_id"] == child["identity"]["publication_id"]
    if mode == "publish_inspection_controlled":
        assert child["identity"]["runtime_attempt_id"] == child["identity"]["publication_id"]
        assert child["identity"]["control_mode"] == "controlled"
    else:
        assert child["identity"]["runtime_attempt_id"] is None
        assert child["identity"]["control_mode"] == "uncontrolled"
    assert child["identity"]["producer_version"] == (
        "unversioned" if mode == "publish_inspection_bad_producer_version" else "local-test")
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config,strategy_allocations,strategy_configs "
                    "FROM trading.live_run_metadata")
        stored, allocations, configs = cur.fetchone()
    assert stored["config_inspection"] == child
    assert allocations == {"TREND": 1.0} and configs is None
    assert {key: stored[key] for key in ("total_capital", "reserve_capital",
               "use_optimization", "use_risk_management")} == {
                   "total_capital": 500000.0, "reserve_capital": 50000.0,
                   "use_optimization": True, "use_risk_management": True}
    if expected_reason == "none":
        assert set(stored) == {"config_inspection", "total_capital", "reserve_capital",
                               "use_optimization", "use_risk_management"}
        assert child["selected_trend"]["strategies"][0]["strategy_id"] == "TREND"
        assert child["selected_trend"]["strategies"][0]["factory_resolved"]["max_history_size"] == 0
        expected_capacity = (2**64 - 1 if mode == "publish_inspection_capacity_uint64" else 2520)
        assert child["selected_trend"]["strategies"][0]["constructor_normalized"]["max_history_size"] == expected_capacity
        if mode == "publish_inspection_controlled":
            fixture = tmp_path / "config-publication-producer-fix2-controlled-fixture.json"
            _write_exact_fixture(fixture, (raw_child + "\n").encode("utf-8"))
            with connection.cursor() as cur:
                cur.execute("""SELECT id,strategy_type,portfolio_id,runtime_revision
                    FROM trading.strategy_registry WHERE id='trend'""")
                registry = cur.fetchone()
                cur.execute("""SELECT portfolio_id FROM trading.strategy_book_memberships
                    WHERE strategy_id='trend' ORDER BY portfolio_id""")
                memberships = [row[0] for row in cur.fetchall()]
                cur.execute("""SELECT date::date::text FROM trading.live_results
                    WHERE strategy_id='LIVE_TREND' AND portfolio_id='BOOK'
                      AND portfolio_type='system' ORDER BY date DESC LIMIT 1""")
                system_result_date = cur.fetchone()[0]
                cur.execute("""SELECT id,action,registry_id,portfolio_id,engine_strategy_id,
                    registry_revision,status FROM trading.runtime_intents ORDER BY id DESC LIMIT 1""")
                intent = cur.fetchone()
                cur.execute("""SELECT id::text,intent_id,registry_revision,run_date::text,
                    status,outcome,publication_id::text FROM trading.runtime_attempts
                    ORDER BY started_at DESC LIMIT 1""")
                attempt = cur.fetchone()
            sidecar = {
                "fixture_sha256": hashlib.sha256((raw_child + "\n").encode("utf-8")).hexdigest(),
                "probe_argv": [str(probe), mode],
                "capture_helper": "build_live_config_inspection_capture",
                "capture_time_provenance": "fixed synthetic probe timestamp",
                "recorded_time_provenance": "database clock_timestamp in final publication transaction",
                "registry": dict(zip(("id", "strategy_type", "primary_book", "runtime_revision"), registry)),
                "membership_books": memberships,
                "system_result_run_date": system_result_date,
                "intent": dict(zip(("id", "action", "registry_id", "portfolio_id",
                                    "engine_strategy_id", "registry_revision", "status"), intent)),
                "attempt": dict(zip(("id", "intent_id", "registry_revision", "run_date",
                                     "status", "outcome", "publication_id"), attempt)),
            }
            _write_exact_fixture(
                fixture.parent / "config-publication-producer-fix2-controlled-provenance.json",
                (json.dumps(sidecar, sort_keys=True, indent=2) + "\n").encode("utf-8"))
    else:
        assert child["supplied"] is None and child["selected_trend"] is None
        assert "synthetic-secret" not in json.dumps(stored)


def _write_exact_fixture(path, raw):
    """Create test evidence without overwriting a prior, possibly different run."""
    try:
        with path.open("xb") as output:
            output.write(raw)
    except FileExistsError:
        assert path.read_bytes() == raw, "refusing different fixture bytes"


def _save_required_publication_fixture(connection, mode, result, child, *, evidence):
    """Capture exact committed JSONB in an explicit test-owned output directory."""
    evidence.mkdir(exist_ok=True)
    with connection.cursor() as cur:
        cur.execute("SELECT (portfolio_config -> 'config_inspection')::text "
                    "FROM trading.live_run_metadata WHERE strategy_id='LIVE_TREND' "
                    "AND portfolio_id='BOOK' AND date='2026-09-22'")
        jsonb_text = cur.fetchone()[0]
        cur.execute("SELECT id,strategy_type,portfolio_id,runtime_revision "
                    "FROM trading.strategy_registry WHERE id='trend'")
        registry = cur.fetchone()
        cur.execute("SELECT portfolio_id FROM trading.strategy_book_memberships "
                    "WHERE strategy_id='trend' ORDER BY portfolio_id")
        memberships = [row[0] for row in cur.fetchall()]
        cur.execute("SELECT date::date::text, to_jsonb(r)::text FROM trading.live_results r "
                    "WHERE strategy_id='LIVE_TREND' AND portfolio_id='BOOK' "
                    "AND portfolio_type='system' ORDER BY date DESC")
        results = cur.fetchall()
        cur.execute("SELECT id::text,intent_id,registry_revision,run_date::text,status,"
                    "outcome,publication_id::text FROM trading.runtime_attempts "
                    "ORDER BY started_at,id")
        attempts = cur.fetchall()
        cur.execute("SELECT id,action,registry_id,portfolio_id,engine_strategy_id,"
                    "registry_revision,status FROM trading.runtime_intents ORDER BY id")
        intents = cur.fetchall()
    assert json.loads(jsonb_text) == child
    raw = jsonb_text.encode("utf-8")
    digest = hashlib.sha256(raw).hexdigest()
    stem = f"{mode}-{digest[:12]}"
    fixture = evidence / f"{stem}.jsonb.txt"
    _write_exact_fixture(fixture, raw)
    assert fixture.read_bytes() == raw
    compact_line = result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0]
    sidecar = {
        "fixture_sha256": digest,
        "fixture_bytes_jsonb_text": len(raw),
        "probe_jsonb_reserialized_compact_bytes": len(compact_line.encode("utf-8")),
        "probe_argv": ["runtime_publication_probe", mode],
        "probe_sha256": hashlib.sha256(Path(result.args[0]).read_bytes()).hexdigest(),
        "capture_helper": "build_live_config_inspection_capture",
        "native_projection_helper": "project_run_consumption or typed unavailable factory",
        "recorded_time_provenance": "transaction clock_timestamp",
        "registry": dict(zip(("id", "strategy_type", "primary_book", "runtime_revision"), registry)),
        "membership_books": memberships,
        "system_results": [{"run_date": row[0], "row_jsonb_text": row[1]} for row in results],
        "intents": [dict(zip(("id", "action", "registry_id", "portfolio_id",
                              "engine_strategy_id", "registry_revision", "status"), row))
                    for row in intents],
        "attempts": [dict(zip(("id", "intent_id", "registry_revision", "run_date",
                               "status", "outcome", "publication_id"), row))
                     for row in attempts],
    }
    provenance = evidence / f"{stem}.provenance.json"
    _write_exact_fixture(provenance,
                         (json.dumps(sidecar, sort_keys=True, indent=2, default=str) + "\n")
                         .encode("utf-8"))
    assert json.loads(provenance.read_text(encoding="utf-8"))["fixture_sha256"] == digest
    return fixture


@pytest.mark.parametrize("controlled", [False, True])
def test_required_publication_seals_typed_unavailable_consumption(connection, controlled, tmp_path):
    """A required begin plus accepted typed evidence must commit v2, even when unavailable."""
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    mode = "publish_required_unavailable" + ("_controlled" if controlled else "")
    if controlled:
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    result = subprocess.run([str(probe), mode],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    child = json.loads(result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    assert child["publication_schema_version"] == 2
    assert child["consumption"]["version"] == 2
    assert child["consumption"]["status"] == "unavailable"
    assert child["consumption"]["reason"] == "instrumentation_missing"
    assert child["consumption"]["nodes"] == []
    assert set(child["consumption"]["coverage"]) == {
        "setup", "market_input", "cost_history", "preparation", "primary",
        "execution", "diagnostics", "control_flow"}
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config -> 'config_inspection' FROM trading.live_run_metadata")
        assert cur.fetchone()[0] == child
    _save_required_publication_fixture(connection, mode, result, child, evidence=tmp_path)


@pytest.mark.parametrize("mode", [
    "required_no_active_attach", "required_unknown_mode", "required_null_output",
    "required_missing_capture", "required_missing_attachment",
    "required_missing_attachment_bad_capture", "required_missing_attachment_oversize",
    "required_wrong_token",
    "required_moved_from", "required_duplicate", "legacy_attachment",
    "required_missing_capture_controlled", "required_missing_attachment_controlled",
    "required_wrong_token_controlled", "required_moved_from_controlled",
    "required_duplicate_controlled", "legacy_attachment_controlled",
])
def test_required_protocol_refusals_cannot_publish_payload(connection, mode):
    """An ignored refusal or absent required evidence cannot commit any queued row."""
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    if "controlled" in mode:
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
    assert f"RUNTIME_PUBLICATION_OK={mode}" in result.stdout
    with connection.cursor() as cur:
        for table in ("positions", "live_results", "live_run_metadata", "risk_limits",
                      "run_inputs", "equity_curve"):
            cur.execute(f"SELECT count(*) FROM trading.{table}")
            assert cur.fetchone()[0] == 0, (mode, table)
        if "controlled" in mode:
            cur.execute("SELECT status FROM trading.runtime_attempts")
            assert [row[0] for row in cur.fetchall()] == ["failed"]


@pytest.mark.parametrize("controlled", [False, True])
def test_required_second_begin_clears_its_output_without_replacing_active_scope(
        connection, controlled):
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    mode = "required_already_active" + ("_controlled" if controlled else "")
    if controlled:
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    result = subprocess.run([str(probe), mode],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    child = json.loads(result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    assert child["publication_schema_version"] == 2
    assert child["consumption"]["reason"] == "instrumentation_missing"
    with connection.cursor() as cur:
        cur.execute("SELECT count(*) FROM trading.live_results")
        assert cur.fetchone()[0] == 1


@pytest.mark.parametrize("case", ["unknown_mode_prevalid", "failure_prevalid",
                                   "stop_prevalid"])
@pytest.mark.parametrize("controlled", [False, True])
def test_required_begin_clears_previously_valid_token_on_nonpublication(
        connection, case, controlled):
    if case == "stop_prevalid" and not controlled:
        pytest.skip("approved stop is a controlled-only operation")
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    mode = "required_" + case + ("_controlled" if controlled else "")
    if controlled and case != "stop_prevalid":
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
    assert f"RUNTIME_PUBLICATION_OK={mode}" in result.stdout
    with connection.cursor() as cur:
        cur.execute("SELECT count(*) FROM trading.live_run_metadata")
        assert cur.fetchone()[0] == 0
        if case == "stop_prevalid":
            cur.execute("SELECT status,outcome FROM trading.runtime_attempts")
            assert cur.fetchone() == ("applied", "stopped")


@pytest.mark.parametrize("mode", [
    "required_stale_after_abandon", "required_stale_after_success",
    "required_cross_database", "required_stale_after_abandon_controlled",
    "required_stale_after_success_controlled", "required_cross_database_controlled",
])
def test_required_stale_or_cross_instance_token_poison_is_lifetime_bound(connection, mode):
    """A same-day replacement cannot accept A's token; fresh C can publish."""
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    if "controlled" in mode:
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
    assert "STABLE_AFTER_STALE=1" in result.stdout
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config -> 'config_inspection' FROM trading.live_run_metadata")
        child = cur.fetchone()[0]
        assert child["publication_schema_version"] == 2
        cur.execute("SELECT count(*) FROM trading.live_results")
        assert cur.fetchone()[0] == (1 if "after_abandon" in mode else 2)
        if "controlled" in mode:
            cur.execute("SELECT status,count(*) FROM trading.runtime_attempts GROUP BY status")
            counts = dict(cur.fetchall())
            assert counts == {"failed": 2 if "after_abandon" in mode else 1,
                              "applied": 1 if "after_abandon" in mode else 2}


@pytest.mark.parametrize("status,controlled", [
    ("complete", False), ("complete", True),
    ("partial", False), ("partial", True),
])
def test_required_generated_native_consumption_round_trips_jsonb(
        connection, status, controlled, tmp_path):
    prepare_exact_publication_schema(connection)
    from algolens.domain.portfolio.consumption_inspection import validate_consumption_v2

    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    mode = f"publish_required_{status}" + ("_controlled" if controlled else "")
    if controlled:
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
    child = json.loads(result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    assert child["publication_schema_version"] == 2
    assert child["identity"]["control_mode"] == (
        "controlled" if controlled else "uncontrolled")
    assert child["consumption"]["status"] == status
    assert child["consumption"]["reason"] == (
        "none" if status == "complete" else "nonfatal_error")
    assert validate_consumption_v2(child["consumption"]) is child["consumption"]
    risk = [node for node in child["consumption"]["nodes"]
            if node["consumer"] == "risk.diagnostics"]
    assert len(risk) == 1
    assert any(read["value"] == "-92233720368.54775808" for read in risk[0]["reads"])
    with connection.cursor() as cur:
        cur.execute("SELECT (portfolio_config -> 'config_inspection')::text "
                    "FROM trading.live_run_metadata")
        assert json.loads(cur.fetchone()[0]) == child
    _save_required_publication_fixture(connection, mode, result, child, evidence=tmp_path)


@pytest.mark.parametrize("case", ["copy_isolation", "moved_to", "after_success"])
@pytest.mark.parametrize("controlled", [False, True])
def test_required_attachment_retains_sealed_copy_and_rejects_after_success(
        connection, case, controlled):
    prepare_exact_publication_schema(connection)
    from algolens.domain.portfolio.consumption_inspection import validate_consumption_v2

    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    mode = f"publish_required_{case}" + ("_controlled" if controlled else "")
    if controlled:
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
    child = json.loads(result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    assert child["consumption"]["status"] == "complete"
    assert validate_consumption_v2(child["consumption"]) is child["consumption"]
    with connection.cursor() as cur:
        cur.execute("SELECT (portfolio_config -> 'config_inspection')::text "
                    "FROM trading.live_run_metadata")
        assert json.loads(cur.fetchone()[0]) == child


@pytest.mark.parametrize("mode,reason", [
    ("publish_required_bad_capture_complete", "capture_failed"),
    ("publish_required_early_unavailable_complete", "projection_invalid"),
])
@pytest.mark.parametrize("controlled", [False, True])
def test_required_early_failure_does_not_erase_valid_final_consumption(
        connection, mode, reason, controlled, tmp_path):
    prepare_exact_publication_schema(connection)
    from algolens.domain.portfolio.consumption_inspection import validate_consumption_v2

    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    mode += "_controlled" if controlled else ""
    if controlled:
        exported = subprocess.run([str(probe),
                                   "snapshot_early_unavailable" if "early_unavailable" in mode
                                   else "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
    child = json.loads(result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    assert child["status"] == "unavailable" and child["reason"] == reason
    assert child["supplied"] is None and child["selected_trend"] is None
    assert child["consumption"]["status"] == "complete"
    assert validate_consumption_v2(child["consumption"]) is child["consumption"]
    assert "synthetic-secret" not in json.dumps(child)
    assert "synthetic-private-rejected" not in json.dumps(child)
    _save_required_publication_fixture(connection, mode, result, child, evidence=tmp_path)


def _publication_payload_snapshot(connection):
    tables = ("positions", "live_results", "live_run_metadata", "risk_limits",
              "run_inputs", "equity_curve", "qt_model_seed_publications")
    with connection.cursor() as cur:
        snapshot = {}
        for table in tables:
            cur.execute(f"SELECT to_jsonb(t)::text FROM trading.{table} t ORDER BY 1")
            snapshot[table] = tuple(row[0] for row in cur.fetchall())
    return snapshot


@pytest.mark.parametrize("case", [
    "required_prior_missing_capture", "required_prior_missing_attachment",
    "required_prior_missing_attachment_bad_capture",
    "required_prior_missing_attachment_oversize", "required_prior_wrong_token",
    "required_prior_moved_from", "required_prior_duplicate",
    "legacy_prior_attachment",
])
@pytest.mark.parametrize("controlled", [False, True])
def test_required_ignored_refusal_preserves_prior_publication_rows(
        connection, case, controlled):
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    suffix = "_controlled" if controlled else ""
    if controlled:
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    seed = subprocess.run([str(probe), "publish_required_complete" + suffix],
                          capture_output=True, text=True, timeout=30)
    assert seed.returncode == 0, (seed.stdout, seed.stderr)
    prior_payload = _publication_payload_snapshot(connection)
    mode = case + suffix
    failed = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert failed.returncode == 0, (mode, failed.returncode, failed.stdout, failed.stderr)
    assert "PRIOR_REFUSAL_STATUS=aborted" in failed.stdout
    assert _publication_payload_snapshot(connection) == prior_payload
    if controlled:
        with connection.cursor() as cur:
            cur.execute("SELECT status,count(*) FROM trading.runtime_attempts GROUP BY status")
            assert dict(cur.fetchall()) == {"applied": 1, "failed": 1}


@pytest.mark.parametrize("controlled", [False, True])
def test_required_in_progress_attach_refusal_rolls_back_prior_payload(
        connection, controlled):
    """A refused reentrant attach during callbacks must abort the whole transaction."""
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    suffix = "_controlled" if controlled else ""
    if controlled:
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    seed = subprocess.run([str(probe), "publish_required_unavailable" + suffix],
                          capture_output=True, text=True, timeout=30)
    assert seed.returncode == 0, (seed.stdout, seed.stderr)
    prior_payload = _publication_payload_snapshot(connection)
    mode = "required_in_progress" + suffix
    failed = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert failed.returncode == 0, (mode, failed.returncode, failed.stdout, failed.stderr)
    assert "IN_PROGRESS_REFUSAL_ROLLED_BACK=1" in failed.stdout
    assert "LATE_ATTACH_ATTEMPTED=1" in failed.stdout
    assert "LATE_ATTACH_REFUSED=1" in failed.stdout
    assert "PUBLICATION_COMMITTED=0" in failed.stdout
    assert _publication_payload_snapshot(connection) == prior_payload
    with connection.cursor() as cur:
        if controlled:
            cur.execute("SELECT status,count(*) FROM trading.runtime_attempts GROUP BY status")
            assert dict(cur.fetchall()) == {"applied": 1, "failed": 1}


@pytest.mark.parametrize("failure", ["callback_failure", "callback_resource_failure"])
@pytest.mark.parametrize("controlled", [False, True])
def test_required_callback_failure_preserves_all_prior_payload_rows(
        connection, failure, controlled):
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    suffix = "_controlled" if controlled else ""
    if controlled:
        exported = subprocess.run([str(probe), "snapshot"], capture_output=True,
                                  text=True, check=True)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    seed = subprocess.run([str(probe), "publish_required_complete" + suffix],
                          capture_output=True, text=True, timeout=30)
    assert seed.returncode == 0, (seed.stdout, seed.stderr)
    prior_payload = _publication_payload_snapshot(connection)
    mode = "required_" + failure + suffix
    failed = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert failed.returncode == 0, (mode, failed.returncode, failed.stdout, failed.stderr)
    if failure == "callback_resource_failure":
        assert "PROJECTOR_ALLOCATION_FAILED=1" in failed.stdout
        assert "RESOURCE_PUBLICATION_STATUS=aborted" in failed.stdout
    else:
        assert "CALLBACK_PUBLICATION_STATUS=aborted" in failed.stdout
    assert _publication_payload_snapshot(connection) == prior_payload
    if controlled:
        with connection.cursor() as cur:
            cur.execute("SELECT status,count(*) FROM trading.runtime_attempts GROUP BY status")
            assert dict(cur.fetchall()) == {"applied": 1, "failed": 1}


@pytest.mark.parametrize("kind", ["jsonb", "compact", "early"])
@pytest.mark.parametrize("controlled", [False, True])
def test_required_combined_transport_capacity_discards_whole_typed_tree(
        connection, kind, controlled):
    """Typed overflow retains or redacts an initially valid capture as bytes require."""
    prepare_exact_publication_schema(connection)
    from algolens.domain.portfolio.consumption_inspection import validate_consumption_v2

    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    mode = f"publish_required_capacity_{kind}" + ("_controlled" if controlled else "")
    if controlled:
        exported = subprocess.run([str(probe),
                                   "snapshot_capacity_early" if kind == "early" else "snapshot"],
                                  capture_output=True, text=True, check=True, timeout=180)
        snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=", 1)[1])
        with connection.cursor() as cur:
            cur.execute("""INSERT INTO trading.runtime_intents
                (registry_id,portfolio_id,engine_strategy_id,action,registry_revision,
                 config_snapshot,status,requested_by,request_reason,approved_by,
                 approval_reason,approved_at)
                VALUES ('trend','BOOK','LIVE_TREND','run',0,%s::jsonb,'approved',
                        '1','synthetic request','2','synthetic approval',now())""",
                        (json.dumps(snapshot),))
    legacy = None
    if kind == "early":
        legacy_mode = "publish_inspection_capacity_early_legacy" + (
            "_controlled" if controlled else "")
        legacy = subprocess.run([str(probe), legacy_mode], capture_output=True,
                                text=True, timeout=180)
        assert legacy.returncode == 0, (
            legacy.returncode, legacy.stdout[-1200:], legacy.stderr)
        legacy_child = json.loads(legacy.stdout.split("CONFIG_PUBLICATION=", 1)[1]
                                  .splitlines()[0])
        assert legacy_child["publication_schema_version"] == 1
        assert legacy_child["status"] == "available"
        assert legacy_child["reason"] == "none"
        assert legacy_child["supplied"] is not None
        assert legacy_child["selected_trend"] is not None
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, (result.returncode, result.stdout[-1200:], result.stderr)
    def line(name):
        return result.stdout.split(name + "=", 1)[1].splitlines()[0]
    child = json.loads(line("CONFIG_PUBLICATION"))
    assert child["identity"]["control_mode"] == (
        "controlled" if controlled else "uncontrolled")
    if kind == "early":
        legacy_capture = legacy.stdout.split("INITIAL_CAPTURE=", 1)[1].splitlines()[0]
        assert line("INITIAL_CAPTURE") == legacy_capture
        assert len(legacy_capture.encode("utf-8")) == int(line("INITIAL_CAPTURE_COMPACT_BYTES"))
        assert line("TYPED_CONSUMPTION_STATUS") == "complete"
        assert int(line("TYPED_CONSUMPTION_NODES")) > 0
        assert int(line("INITIAL_CAPTURE_COMPACT_BYTES")) <= 2_097_152
        assert 0 < int(line("INITIAL_CAPTURE_FIELDS")) <= 20_000
    else:
        candidate = json.loads(line("CONSUMPTION_CANDIDATE"))
        assert candidate["status"] == "complete"
        assert candidate["nodes"]
    if kind == "jsonb":
        assert int(line("CANDIDATE_COMPACT_BYTES")) <= 2_097_152
        assert int(line("CANDIDATE_JSONB_BYTES")) > 2_097_152
    else:
        assert (int(line("CANDIDATE_COMPACT_BYTES")) > 2_097_152 or
                int(line("CANDIDATE_JSONB_BYTES")) > 2_097_152)
    if kind == "compact":
        assert int(line("CANDIDATE_COMPACT_BYTES")) > 2_097_152
    if kind == "early":
        assert (int(line("FALLBACK_COMPACT_BYTES")) > 2_097_152 or
                int(line("FALLBACK_JSONB_BYTES")) > 2_097_152)
    else:
        assert int(line("FALLBACK_COMPACT_BYTES")) <= 2_097_152
        assert int(line("FALLBACK_JSONB_BYTES")) <= 2_097_152
    assert int(line("COMMITTED_COMPACT_BYTES")) <= 2_097_152
    assert int(line("COMMITTED_JSONB_BYTES")) <= 2_097_152
    assert child["publication_schema_version"] == 2
    if kind == "early":
        assert child["status"] == "unavailable" and child["reason"] == "capture_failed"
        assert child["supplied"] is None and child["selected_trend"] is None
    else:
        assert child["status"] == "available" and child["reason"] == "none"
    assert child["consumption"]["status"] == "unavailable"
    assert child["consumption"]["reason"] == "capacity_exceeded"
    assert child["consumption"]["nodes"] == []
    assert validate_consumption_v2(child["consumption"]) is child["consumption"]
    assert {stage["status"] for stage in child["consumption"]["coverage"].values()} == {
        "unavailable"}
    assert {stage["reason"] for stage in child["consumption"]["coverage"].values()} == {
        "capacity_exceeded"}
    with connection.cursor() as cur:
        cur.execute("SELECT (portfolio_config -> 'config_inspection')::text "
                    "FROM trading.live_run_metadata")
        actual_jsonb_text = cur.fetchone()[0]
        assert json.loads(actual_jsonb_text) == child
        assert len(actual_jsonb_text.encode("utf-8")) == int(line("COMMITTED_JSONB_BYTES"))
        if controlled:
            cur.execute("SELECT status,outcome FROM trading.runtime_attempts")
            assert cur.fetchall() == [("applied", "published")] * (
                2 if kind == "early" else 1)
    print(f"CAPACITY_MEASUREMENT mode={mode} "
          f"initial_capture_compact={line('INITIAL_CAPTURE_COMPACT_BYTES') if kind == 'early' else 'none'} "
          f"candidate_compact={line('CANDIDATE_COMPACT_BYTES')} "
          f"candidate_jsonb={line('CANDIDATE_JSONB_BYTES')} "
          f"fallback_compact={line('FALLBACK_COMPACT_BYTES')} "
          f"fallback_jsonb={line('FALLBACK_JSONB_BYTES')} "
          f"committed_compact_reserialized={line('COMMITTED_COMPACT_BYTES')} "
          f"committed_jsonb={line('COMMITTED_JSONB_BYTES')} "
          f"legacy_same_capture_admitted={kind == 'early'}", flush=True)


@pytest.mark.parametrize("mode", [
    "publish_inspection_fix1_omitted_type",
    "publish_inspection_fix1_descriptor_promotion",
    "publish_inspection_fix1_wrong_reason",
    "publish_inspection_fix1_wrong_condition",
    "publish_inspection_fix1_wrong_unit",
    "publish_inspection_fix1_wrong_origin",
    "publish_inspection_fix1_wrong_type",
    "publish_inspection_fix1_wrong_state",
    "publish_inspection_fix1_integer_underflow",
    "publish_inspection_fix1_pair_underflow",
    "publish_inspection_fix1_float_capture_version",
    "publish_inspection_fix1_float_projection_version",
    "publish_inspection_fix1_float_selected_version",
    "publish_inspection_fix1_allocation_changed",
    "publish_inspection_fix1_missing_selected",
    "publish_inspection_fix1_extra_selected",
    "publish_inspection_fix1_bad_month",
    "publish_inspection_fix1_bad_day",
    "publish_inspection_fix1_bad_hour",
    "publish_inspection_fix1_bad_minute",
    "publish_inspection_fix1_bad_second",
    "publish_inspection_fix1_bad_leap",
    "publish_inspection_fix1_bad_year",
])
def test_fix1_malformed_capture_is_redacted_but_results_publish(connection, mode):
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (mode, result.stdout, result.stderr)
    assert f"RUNTIME_PUBLICATION_OK={mode}" in result.stdout
    child = json.loads(result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    assert child["status"] == "unavailable"
    assert child["reason"] == "capture_failed"
    assert child["supplied"] is None and child["selected_trend"] is None
    assert child["identity"]["registry_id"] == "trend"
    assert child["identity"]["engine_strategy_id"] == "LIVE_TREND"
    assert child["identity"]["portfolio_id"] == "BOOK"
    assert child["identity"]["run_date"] == "2026-09-22"
    assert child["identity"]["capture_id"] == child["identity"]["publication_id"]
    if "bad_" in mode and any(part in mode for part in (
            "month", "day", "hour", "minute", "second", "leap", "year")):
        assert child["captured_at"] == child["publication_recorded_at"]
    else:
        assert child["captured_at"] == "2026-09-22T12:34:56.123456Z"
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config,strategy_allocations FROM trading.live_run_metadata")
        stored, allocations = cur.fetchone()
        cur.execute("SELECT count(*) FROM trading.live_results WHERE strategy_id='LIVE_TREND' "
                    "AND portfolio_id='BOOK' AND portfolio_type='system' AND date='2026-09-22'")
        result_count = cur.fetchone()[0]
    assert stored["config_inspection"] == child
    assert result_count == 1
    assert {key: stored[key] for key in ("total_capital", "reserve_capital",
               "use_optimization", "use_risk_management")} == {
                   "total_capital": 500000.0, "reserve_capital": 50000.0,
                   "use_optimization": True, "use_risk_management": True}
    if mode == "publish_inspection_fix1_allocation_changed":
        assert allocations == {"TREND": 0.5}
    if mode == "publish_inspection_fix1_missing_selected":
        assert allocations == {"TREND": 1.0, "OTHER": 0.5}
    if mode == "publish_inspection_fix1_extra_selected":
        assert allocations == {}
    assert "synthetic-private-omitted-type" not in json.dumps(stored)


def test_fix1_valid_leap_day_capture_stays_available(connection):
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    result = subprocess.run([str(probe), "publish_inspection_fix1_valid_leap"],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.stdout, result.stderr)
    child = json.loads(result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    assert child["status"] == "available" and child["reason"] == "none"
    assert child["captured_at"] == "2000-02-29T12:34:56.123456Z"
    assert child["captured_at"] != child["publication_recorded_at"]
    with connection.cursor() as cur:
        cur.execute("SELECT count(*) FROM trading.live_results WHERE strategy_id='LIVE_TREND' "
                    "AND portfolio_id='BOOK' AND portfolio_type='system' AND date='2026-09-22'")
        assert cur.fetchone()[0] == 1


@pytest.mark.parametrize("mode,status,selected,allocation", [
    ("publish_inspection_fix2_integer_integer", "available", 1, 1),
    ("publish_inspection_fix2_integer_float", "available", 1, 1.0),
    ("publish_inspection_fix2_float_integer", "available", 1.0, 1),
    ("publish_inspection_fix2_float_float", "available", 1.0, 1.0),
    ("publish_inspection_fix2_zero_mixed", "available", 0, -0.0),
    ("publish_inspection_fix2_negative_mixed", "available", -1, -1.0),
    ("publish_inspection_fix2_signed_unsigned", "available", 1, 1),
    ("publish_inspection_fix2_large_equal_integer", "available", 9007199254740993,
     9007199254740993),
    ("publish_inspection_fix2_large_unequal_mixed", "unavailable", None,
     9007199254740992.0),
    ("publish_inspection_fix2_large_unequal_mixed_inverse", "unavailable", None,
     9007199254740993),
    ("publish_inspection_fix2_negative_unsigned_mismatch", "unavailable", None, 1),
    ("publish_inspection_fix2_nonfinite", "unavailable", None, 1.0),
    ("publish_inspection_fix2_non_number", "unavailable", None, 1.0),
])
def test_fix2_allocation_numeric_equality_preserves_publication(
        connection, mode, status, selected, allocation):
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (mode, result.stdout, result.stderr)
    assert f"RUNTIME_PUBLICATION_OK={mode}" in result.stdout
    child = json.loads(result.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    assert child["status"] == status
    assert child["reason"] == ("none" if status == "available" else "capture_failed")
    assert child["identity"]["registry_id"] == "trend"
    assert child["identity"]["portfolio_id"] == "BOOK"
    assert child["identity"]["run_date"] == "2026-09-22"
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config,strategy_allocations FROM trading.live_run_metadata")
        metadata, stored_allocations = cur.fetchone()
        cur.execute("SELECT count(*) FROM trading.live_results WHERE strategy_id='LIVE_TREND' "
                    "AND portfolio_id='BOOK' AND portfolio_type='system' AND date='2026-09-22'")
        assert cur.fetchone()[0] == 1
    assert metadata["config_inspection"] == child
    assert stored_allocations == {"TREND": allocation}
    assert {key: metadata[key] for key in ("total_capital", "reserve_capital",
           "use_optimization", "use_risk_management")} == {
               "total_capital": 500000.0, "reserve_capital": 50000.0,
               "use_optimization": True, "use_risk_management": True}
    if status == "available":
        actual = child["selected_trend"]["strategies"][0]["selected_allocation"]
        assert actual == selected and type(actual) is type(selected)
        assert child["selected_trend"]["strategies"][0]["strategy_id"] == "TREND"
    else:
        assert child["supplied"] is None and child["selected_trend"] is None
        assert "synthetic-not-a-number" not in json.dumps(metadata)


def test_fix2_projector_resource_failure_aborts_and_keeps_prior_publication(connection):
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    first = subprocess.run([str(probe), "publish_inspection"],
                           capture_output=True, text=True, timeout=30)
    assert first.returncode == 0, (first.stdout, first.stderr)
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config,strategy_allocations,strategy_configs,created_at "
                    "FROM trading.live_run_metadata WHERE date='2026-09-22' "
                    "AND strategy_id='LIVE_TREND' AND portfolio_id='BOOK'")
        prior_metadata = cur.fetchone()
        cur.execute("SELECT to_jsonb(r) FROM trading.live_results r "
                    "WHERE date='2026-09-22' AND strategy_id='LIVE_TREND' "
                    "AND portfolio_id='BOOK' AND portfolio_type='system'")
        prior_result = cur.fetchone()[0]
    assert prior_metadata[0]["total_capital"] == 500000.0
    assert prior_result["total_pnl"] == 123
    second = subprocess.run([str(probe), "publish_inspection_fix2_resource_failure"],
                            capture_output=True, text=True, timeout=30)
    assert second.returncode == 0, (second.stdout, second.stderr)
    assert "PROJECTOR_ALLOCATION_FAILED=1" in second.stdout
    assert "RESOURCE_PUBLICATION_STATUS=aborted" in second.stdout
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config,strategy_allocations,strategy_configs,created_at "
                    "FROM trading.live_run_metadata WHERE date='2026-09-22' "
                    "AND strategy_id='LIVE_TREND' AND portfolio_id='BOOK'")
        assert cur.fetchone() == prior_metadata
        cur.execute("SELECT to_jsonb(r) FROM trading.live_results r "
                    "WHERE date='2026-09-22' AND strategy_id='LIVE_TREND' "
                    "AND portfolio_id='BOOK' AND portfolio_type='system'")
        assert cur.fetchone()[0] == prior_result


def test_reserved_capture_refusals_and_old_writer_replacement(connection):
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    for mode in ("inspection_direct", "inspection_repeat", "inspection_mismatch"):
        result = subprocess.run([str(probe), mode], capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, (mode, result.stdout, result.stderr)
        assert f"RUNTIME_PUBLICATION_OK={mode}" in result.stdout
    with connection.cursor() as cur:
        cur.execute("SELECT count(*) FROM trading.live_run_metadata")
        assert cur.fetchone()[0] == 0
    first = subprocess.run([str(probe), "publish_inspection"], capture_output=True, text=True, timeout=30)
    assert first.returncode == 0, (first.stdout, first.stderr)
    initial = json.loads(first.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    with connection.cursor() as cur:
        cur.execute("SELECT created_at FROM trading.live_run_metadata")
        created_at = cur.fetchone()[0]
    second = subprocess.run([str(probe), "publish_inspection"], capture_output=True, text=True, timeout=30)
    assert second.returncode == 0, (second.stdout, second.stderr)
    recaptured = json.loads(second.stdout.split("CONFIG_PUBLICATION=", 1)[1].splitlines()[0])
    assert recaptured["identity"]["capture_id"] != initial["identity"]["capture_id"]
    with connection.cursor() as cur:
        cur.execute("SELECT created_at FROM trading.live_run_metadata")
        assert cur.fetchone()[0] == created_at
    legacy = subprocess.run([str(probe), "publish_inspection_legacy_writer"],
                            capture_output=True, text=True, timeout=30)
    assert legacy.returncode == 0, (legacy.stdout, legacy.stderr)
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config FROM trading.live_run_metadata")
        assert "config_inspection" not in cur.fetchone()[0]


@pytest.mark.parametrize("failed_mode", ["rollback_inspection", "retire_inspection"])
def test_failed_publication_keeps_last_committed_inspection(connection, failed_mode):
    prepare_exact_publication_schema(connection)
    probe = Path("/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe")
    first = subprocess.run([str(probe), "publish_inspection"], capture_output=True, text=True, timeout=30)
    assert first.returncode == 0, (first.stdout, first.stderr)
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config,created_at FROM trading.live_run_metadata")
        prior = cur.fetchone()
    failed = subprocess.run([str(probe), failed_mode], capture_output=True, text=True, timeout=30)
    assert failed.returncode == 0, (failed.stdout, failed.stderr)
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_config,created_at FROM trading.live_run_metadata")
        assert cur.fetchone() == prior


def test_real_http_approval_cpp_publication_and_http_applied_status(connection, monkeypatch, tmp_path):
    prepare_exact_publication_schema(connection)
    from flask import Flask
    from flask_jwt_extended import JWTManager, create_access_token, get_csrf_token
    from psycopg2.extras import RealDictCursor
    from algolens.adapters.http import portfolio as portfolio_http
    from algolens.adapters.http import runtime_control as runtime_http
    from algolens.application.runtime_control import RuntimeControlService
    from algolens.infrastructure.config.runtime_control import RuntimeControlConfig
    from algolens.infrastructure.portfolio.runtime_control import PostgresRuntimeControlRepository
    from tests.conftest import InMemoryCurrentUsers

    probe = "/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe"
    exported = subprocess.run([probe,"snapshot"], capture_output=True, text=True, check=True)
    snapshot = json.loads(exported.stdout.split("RUNTIME_SNAPSHOT=",1)[1])
    manifest = tmp_path / "reviewed-synthetic-config.json"
    manifest.write_text(json.dumps({"version":1,"scopes":[{
        "registry_id":"trend","portfolio_id":"BOOK","engine_strategy_id":"LIVE_TREND",
        "config_snapshot":snapshot}]}))
    config = RuntimeControlConfig({"QT_RUNTIME_CONTROL_ENABLED":"true",
        "QT_RUNTIME_APPROVER_IDS":"8","QT_RUNTIME_CONFIG_MANIFEST":str(manifest)})
    repository = PostgresRuntimeControlRepository(lambda: psycopg2.connect(
        os.environ["ALGOLENS_TEST_DB"], cursor_factory=RealDictCursor))
    monkeypatch.setattr(runtime_http,"_service",lambda: RuntimeControlService(repository,config))
    users = InMemoryCurrentUsers()
    monkeypatch.setattr(portfolio_http,"create_identity_dependencies",lambda: (users,object(),object()))
    app = Flask(__name__)
    app.config.update(TESTING=True,JWT_SECRET_KEY="synthetic-runtime-test-secret-only-2026",
        JWT_TOKEN_LOCATION=["cookies"],JWT_COOKIE_CSRF_PROTECT=True)
    JWTManager(app)
    app.register_blueprint(runtime_http.runtime_control_bp,url_prefix="/portfolio")
    client = app.test_client()
    def login(identity,role):
        users.set(identity,role=role)
        with app.app_context():
            token = create_access_token(identity=identity)
            csrf = get_csrf_token(token)
        client.set_cookie("access_token_cookie",token)
        return {"X-CSRF-TOKEN":csrf}
    base = "/portfolio/strategies/trend/runtime"
    response = client.post(base+"/requests",json={"action":"run","portfolio_id":"BOOK","reason":"Review"},
                           headers=login("7","general_member"))
    assert response.status_code == 201, response.json
    intent = response.json["intent"]["id"]
    response = client.post(f"{base}/requests/{intent}/approve",json={"reason":"Exact capital reviewed"},
                           headers=login("8","admin"))
    assert response.status_code == 200, response.json
    result = subprocess.run([probe,"publish_controlled"],capture_output=True,text=True,timeout=30)
    assert result.returncode == 0, result.stdout[-1000:]+result.stderr[-500:]
    status = client.get(base+"?portfolio_id=BOOK")
    assert status.status_code == 200, status.json
    assert status.json["latest_attempt"]["status"] == "applied"
    assert status.json["latest_attempt"]["outcome"] == "published"
    assert status.json["latest_attempt"]["publication_id"]
    assert "config_snapshot" not in status.json["intent"]


@pytest.mark.parametrize("first", ["api", "publisher"])
def test_real_qt_api_and_cpp_publisher_serialize_in_both_orders(connection, monkeypatch, first):
    prepare_exact_publication_schema(connection)
    from flask import Flask
    from flask_jwt_extended import JWTManager, create_access_token, get_csrf_token
    from psycopg2.extras import RealDictCursor
    from algolens.adapters.http import portfolio as portfolio_http
    from algolens.adapters.http import qt_workflow as qt_http
    from algolens.infrastructure.config.dependencies import create_qt_decision_read_service
    from algolens.infrastructure.portfolio.instrument_catalog import PostgresInstrumentCatalog
    from algolens.infrastructure.portfolio import repositories
    from algolens.infrastructure.portfolio.strategy_registry import PostgresStrategyRegistry
    from tests.conftest import InMemoryCurrentUsers

    factory = lambda: psycopg2.connect(os.environ["ALGOLENS_TEST_DB"], cursor_factory=RealDictCursor)
    # This is the explicit legacy cutover case, with real disabled capability
    # checked by both the HTTP reader and the existing locked repository.
    with connection.cursor() as cur:
        cur.execute("CREATE SCHEMA IF NOT EXISTS auth; "
                    "CREATE TABLE IF NOT EXISTS auth.users (id bigint PRIMARY KEY,role text NOT NULL); "
                    "INSERT INTO auth.users VALUES (8,'admin') ON CONFLICT(id) DO UPDATE SET role='admin'")
        api_migration = Path(__file__).parents[3] / "algolens-qt/algolens-api/migrations/003_qt_decision_workflow.sql"
        cur.execute(api_migration.read_text())
        # The legacy-write guard locks registry rows, which now selects asset_class (installed API 007).
        asset_class = api_migration.with_name("007_strategy_registry_asset_class.sql")
        assert hashlib.sha256(asset_class.read_bytes()).hexdigest() == \
            "908b03d741d9324f655101de04e738d61af1c93934fcabed8b84c08c0eec4d52"
        cur.execute(asset_class.read_text())
        cur.execute("INSERT INTO trading.qt_workflow_capabilities (book_id,enabled,version) "
                    "VALUES ('BOOK',false,1)")
        cur.execute('CREATE SCHEMA IF NOT EXISTS metadata; '
                    'CREATE TABLE IF NOT EXISTS metadata.contract_metadata '
                    '("Asset Type" text,"Databento Symbol" text,"IB Symbol" text); '
                    'TRUNCATE metadata.contract_metadata; '
                    'INSERT INTO metadata.contract_metadata VALUES (\'FUTURE\',\'ES\',\'ES\')')
    monkeypatch.setattr(qt_http,"_read_service",lambda: create_qt_decision_read_service(connection_factory=factory))
    repository = repositories.PostgresPortfolioRepository(connection_factory=factory)
    registry = PostgresStrategyRegistry(connection_factory=factory)
    monkeypatch.setattr(portfolio_http,"create_portfolio_dependencies",lambda: (registry,repository))
    monkeypatch.setattr(portfolio_http,"create_market_data",lambda: None)
    monkeypatch.setattr(portfolio_http,"create_instrument_catalog",lambda: PostgresInstrumentCatalog(factory))
    monkeypatch.setattr(repositories,"current_utc_date",lambda: date(2026,9,22))
    users = InMemoryCurrentUsers()
    users.set("8",role="admin")
    monkeypatch.setattr(portfolio_http,"create_identity_dependencies",lambda: (users,object(),object()))
    monkeypatch.setattr(qt_http,"create_identity_dependencies",lambda: (users,object(),object()))
    app = Flask(__name__)
    app.config.update(TESTING=True,JWT_SECRET_KEY="synthetic-concurrency-secret-only-2026",
        JWT_TOKEN_LOCATION=["cookies"],JWT_COOKIE_CSRF_PROTECT=True)
    JWTManager(app)
    app.register_blueprint(portfolio_http.portfolio_bp,url_prefix="/portfolio")
    with app.app_context():
        token = create_access_token(identity="8")
        csrf = get_csrf_token(token)
    def edit():
        with app.test_client() as client:
            client.set_cookie("access_token_cookie",token)
            response = client.post("/portfolio/positions",json={"strategy_id":"trend",
                "portfolio_id":"BOOK","strategy_name":"TREND","symbol":"ES","quantity":0,
                "reason":"Synthetic concurrency closure","acknowledge_risk":True},
                headers={"X-CSRF-TOKEN":csrf})
            return response.status_code,response.json
    with connection.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
            (strategy_id,portfolio_id,strategy_name,date,symbol,portfolio_type,quantity,average_price,
             daily_unrealized_pnl,daily_realized_pnl,last_update)
            SELECT 'LIVE_TREND','BOOK','TREND','2026-09-22','ES',stream,12,100,0,0,'2026-09-22'::timestamptz
            FROM unnest(ARRAY['system','qt']) stream""")

    api_locked, release_api = threading.Event(), threading.Event()
    real_lock = repositories.acquire_qt_book_locks
    def paused_lock(cursor,*books):
        real_lock(cursor,*books)
        api_locked.set()
        assert release_api.wait(15), "test failed to release API lock"
    if first == "api":
        monkeypatch.setattr(repositories,"acquire_qt_book_locks",paused_lock)
    else:
        # Owned-fixture-only latch: pause inside the short final transaction,
        # after its real registry/book locks. No hook exists in production code.
        with connection.cursor() as cur:
            cur.execute("""SELECT pg_advisory_lock(90220922);
                CREATE FUNCTION trading.test_publish_latch() RETURNS trigger LANGUAGE plpgsql AS $$
                BEGIN PERFORM pg_advisory_xact_lock(90220922); RETURN NEW; END $$;
                CREATE TRIGGER test_publish_latch BEFORE INSERT ON trading.risk_limits
                FOR EACH ROW EXECUTE FUNCTION trading.test_publish_latch()""")

    def wait_for_blocked(query_fragment):
        deadline = time.monotonic()+10
        while time.monotonic()<deadline:
            with connection.cursor() as cur:
                cur.execute("""SELECT EXISTS (SELECT 1 FROM pg_stat_activity
                    WHERE pid<>pg_backend_pid() AND datname=current_database()
                    AND cardinality(pg_blocking_pids(pid))>0 AND query ILIKE %s)""",
                    ("%"+query_fragment+"%",))
                if cur.fetchone()[0]:
                    return
            time.sleep(.02)
        raise AssertionError("competing writer did not block on the held lock")

    probe = "/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe"
    process = subprocess.Popen([probe,"publish_concurrent"],stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    pool = ThreadPoolExecutor(max_workers=2)
    try:
        def ready():
            for line in process.stdout:
                if line.strip()=="RUNTIME_READY":
                    return True
            return False
        assert pool.submit(ready).result(timeout=15)
        if first == "api":
            edit_future = pool.submit(edit)
            assert api_locked.wait(10)
            process.stdin.write("publish\n")
            process.stdin.flush()
            wait_for_blocked("SELECT runtime_revision")
            assert process.poll() is None
            release_api.set()
        else:
            process.stdin.write("publish\n")
            process.stdin.flush()
            wait_for_blocked("INSERT INTO trading.risk_limits")
            edit_future = pool.submit(edit)
            wait_for_blocked("FROM trading.strategy_registry")
            assert not edit_future.done()
            with connection.cursor() as cur:
                cur.execute("SELECT pg_advisory_unlock(90220922)")
        status,payload = edit_future.result(timeout=15)
        assert status==201,payload
        stdout,stderr = process.communicate(timeout=15)
        assert process.returncode==0,stdout[-1000:]+stderr[-500:]
        with connection.cursor() as cur:
            cur.execute("SELECT quantity FROM trading.positions WHERE portfolio_type='qt'")
            assert cur.fetchone()[0]==0, "engine seed reopened the API's explicit zero"
            cur.execute("SELECT count(*) FROM trading.position_overrides")
            assert cur.fetchone()[0]==1
    finally:
        release_api.set()
        with connection.cursor() as cur:
            cur.execute("SELECT pg_advisory_unlock_all()")
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=5)
        pool.shutdown(wait=True)
