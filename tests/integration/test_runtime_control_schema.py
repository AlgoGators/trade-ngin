"""Migration 013 against the owned, network-none PostgreSQL fixture only."""
import os
import subprocess
import json
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
              date timestamptz, total_pnl numeric, config jsonb,current_portfolio_value numeric,
              total_annualized_return numeric,volatility numeric,total_cumulative_return numeric);
            CREATE TABLE trading.equity_curve (strategy_id text, portfolio_id text,
              timestamp timestamptz, equity numeric, portfolio_type text,
              UNIQUE(portfolio_id,strategy_id,timestamp,portfolio_type));
            CREATE TABLE trading.executions (strategy_id text, portfolio_id text);
            CREATE TABLE trading.signals (strategy_id text, portfolio_id text);
            CREATE TABLE trading.live_run_metadata (strategy_id text, portfolio_id text,
              date date, strategy_allocations jsonb,portfolio_config jsonb,strategy_configs jsonb,
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


def test_real_http_approval_cpp_publication_and_http_applied_status(connection, monkeypatch, tmp_path):
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
    from flask import Flask
    from flask_jwt_extended import JWTManager, create_access_token, get_csrf_token
    from psycopg2.extras import RealDictCursor
    from algolens.adapters.http import portfolio as portfolio_http
    from algolens.infrastructure.portfolio import repositories
    from algolens.infrastructure.portfolio.strategy_registry import PostgresStrategyRegistry
    from tests.conftest import InMemoryCurrentUsers

    factory = lambda: psycopg2.connect(os.environ["ALGOLENS_TEST_DB"], cursor_factory=RealDictCursor)
    repository = repositories.PostgresPortfolioRepository(connection_factory=factory)
    registry = PostgresStrategyRegistry(connection_factory=factory)
    monkeypatch.setattr(portfolio_http,"create_portfolio_dependencies",lambda: (registry,repository))
    monkeypatch.setattr(portfolio_http,"create_market_data",lambda: None)
    monkeypatch.setattr(repositories,"current_utc_date",lambda: date(2026,9,22))
    users = InMemoryCurrentUsers()
    users.set("8",role="admin")
    monkeypatch.setattr(portfolio_http,"create_identity_dependencies",lambda: (users,object(),object()))
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
            SELECT 'LIVE_TREND','BOOK','TREND','2026-09-22','ES',stream,12,100,0,0,now()
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
