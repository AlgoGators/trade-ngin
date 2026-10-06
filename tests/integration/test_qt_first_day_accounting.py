"""Real first-day QT transactions on an explicitly owned synthetic PostgreSQL.

The opening System rows and market prices are synthetic; the native evaluator,
anchor, processor, rollback and receipt verification are production code.
"""
from datetime import timedelta, timezone
from copy import deepcopy
from decimal import Decimal
import json
import os
import subprocess

import pytest
from psycopg2.extras import Json

from test_runtime_control_schema import connection as base_connection
from test_qt_desk_storage import (desk, ROOT, BINARY, DECISION, ATTEMPT, MODEL,
    qt_digest_v1, internal_snapshot_digest)
from test_qt_desk_accounting import all_state

PROBE = BINARY.with_name("qt_desk_upstream_probe")
ANCHOR = "a1000000-0000-4000-8000-000000000001"
MARKET = "a2000000-0000-4000-8000-000000000001"
INPUT = "a3000000-0000-4000-8000-000000000001"


def owned_clock_identity(conn):
    """Permit catalog-clock control only inside the private synthetic socket."""
    import re
    import stat
    from pathlib import Path
    from psycopg2.extensions import parse_dsn
    params = parse_dsn(os.environ['ALGOLENS_TEST_DB'])
    assert set(params) <= {'host', 'port', 'dbname', 'user'}
    assert re.fullmatch(r'algolens_test_[A-Za-z0-9_]+', params['dbname'])
    root = Path(params['host'])
    assert root.parent == Path('/tmp')
    assert re.fullmatch(r'algolens-repair-pg-[A-Za-z0-9_-]+\.[A-Za-z0-9]+', root.name)
    assert root.resolve() == root and not root.is_symlink()
    directory = root.stat()
    assert directory.st_uid == os.getuid() and stat.S_IMODE(directory.st_mode) == 0o700
    data = root / 'data'
    assert data.resolve() == data and not data.is_symlink()
    assert data.stat().st_uid == os.getuid() and stat.S_IMODE(data.stat().st_mode) == 0o700
    with conn.cursor() as cur:
        cur.execute("SELECT current_database(),current_user,inet_server_addr(),"
                    "current_setting('unix_socket_directories'),current_setting('port'),"
                    "(SELECT rolsuper FROM pg_roles WHERE rolname=current_user),"
                    "current_setting('data_directory'),current_setting('listen_addresses')")
        database, user, network, socket_directory, port, privileged, data_directory, listen = cur.fetchone()
        assert (database,user,network,socket_directory,privileged) == (
            params['dbname'],params['user'],None,str(root),True)
        assert params.get('port',port) == port
        assert data_directory == str(data) and listen == ''
        socket = (root/('.s.PGSQL.'+port)).stat()
        assert stat.S_ISSOCK(socket.st_mode) and socket.st_uid == os.getuid()
        cur.execute("SELECT p.oid,p.prosrc,l.lanname FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang "
                    "WHERE p.oid='pg_catalog.clock_timestamp()'::regprocedure")
        identity = cur.fetchone()
        assert identity[1:] == ('clock_timestamp','internal')
        cur.execute("SELECT to_regprocedure('pg_catalog.qt_test_actual_clock_timestamp()') IS NULL")
        assert cur.fetchone() == (True,)
    return identity


@pytest.fixture()
def connection(request):
    """Synthetic database clock only; preserve the real builtin and restore it.

    The catalog wrapper survives native security code's pg_catalog search path.
    It is never installed until the root/socket/database/user/no-TCP guard passes.
    The disposable trading schema is discarded on teardown before removing the
    wrapper; the original clock function OID and implementation are restored.
    """
    from psycopg2 import sql
    fixture = base_connection.__wrapped__()
    conn = next(fixture)
    shifted = "finalizes_on_day_two" in request.node.name
    installed = False
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT current_database()")
            database = cur.fetchone()[0]
            zone = getattr(request, 'param', 'UTC')
            cur.execute(sql.SQL("ALTER DATABASE {} SET timezone={}").format(sql.Identifier(database),sql.Literal(zone)))
            cur.execute("SET TIME ZONE %s", (zone,))
        if shifted:
            original = owned_clock_identity(conn)
            # Prove reversible catalog permission before installing any wrapper.
            with conn.cursor() as cur:
                cur.execute("BEGIN")
                try:
                    cur.execute("ALTER FUNCTION pg_catalog.clock_timestamp() RENAME TO qt_test_actual_clock_timestamp")
                finally:
                    cur.execute("ROLLBACK")
            assert owned_clock_identity(conn) == original
            with conn.cursor() as cur:
                cur.execute("BEGIN")
                try:
                    cur.execute("ALTER FUNCTION pg_catalog.clock_timestamp() RENAME TO qt_test_actual_clock_timestamp")
                    cur.execute("CREATE FUNCTION pg_catalog.clock_timestamp() RETURNS timestamptz LANGUAGE sql VOLATILE "
                        "AS $$ SELECT pg_catalog.qt_test_actual_clock_timestamp() $$")
                    cur.execute("COMMIT")
                    installed = True
                except BaseException:
                    cur.execute("ROLLBACK")
                    raise
        yield conn
    finally:
        try:
            if installed:
                with conn.cursor() as cur:
                    cur.execute("BEGIN")
                    try:
                        cur.execute("DROP SCHEMA trading CASCADE")
                        cur.execute("DROP FUNCTION pg_catalog.clock_timestamp()")
                        cur.execute("ALTER FUNCTION pg_catalog.qt_test_actual_clock_timestamp() RENAME TO clock_timestamp")
                        cur.execute("COMMIT")
                    except BaseException:
                        cur.execute("ROLLBACK")
                        raise
                assert owned_clock_identity(conn) == original
            with conn.cursor() as cur:
                cur.execute(sql.SQL("ALTER DATABASE {} RESET timezone").format(sql.Identifier(database)))
        finally:
            # Close even when restoration fails; pytest retains that failure.
            next(fixture, None)


def invoke(*args, payload=None):
    return subprocess.run([str(PROBE), *args],
        input=None if payload is None else json.dumps(payload),
        text=True, capture_output=True, timeout=30, env=os.environ.copy())


def state(conn):
    result = all_state(conn)
    with conn.cursor() as cur:
        for table in ("qt_desk_accounting_inputs", "qt_desk_finalizations",
                      "qt_desk_finalization_sources", "qt_first_day_anchors"):
            cur.execute("SELECT to_jsonb(t)::text FROM trading." + table + " t ORDER BY to_jsonb(t)::text")
            result.append(cur.fetchall())
    return result


@pytest.fixture()
def first_day(desk):
    conn, _ = desk
    with conn.cursor() as cur:
        for migration in ("017_desk_accounting_inputs.sql", "018_qt_desk_finalization.sql",
                          "030_qt_first_day_bootstrap.sql"):
            cur.execute((ROOT / "migrations" / migration).read_text())
        cur.execute("ALTER TABLE trading.executions ADD strategy_name text,ADD date date,ADD symbol text,"
            "ADD exec_id text,ADD order_id text,ADD side text,ADD quantity numeric,ADD price numeric,"
            "ADD execution_time timestamptz,ADD commissions_fees numeric,ADD implicit_price_impact numeric,"
            "ADD slippage_market_impact numeric,ADD total_transaction_costs numeric,ADD is_partial boolean;"
            "ALTER TABLE trading.live_results ADD daily_pnl numeric,ADD daily_realized_pnl numeric,"
            "ADD daily_unrealized_pnl numeric,ADD daily_transaction_costs numeric,ADD total_transaction_costs numeric")
        cur.execute("SELECT source_day,clock_timestamp() FROM trading.qt_decisions WHERE decision_id=%s", (DECISION,))
        day, now = cur.fetchone()
        cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,portfolio_type,total_pnl,"
            "current_portfolio_value,daily_pnl,daily_realized_pnl,daily_unrealized_pnl,daily_transaction_costs,total_transaction_costs) "
            "VALUES('BOOK','LIVE_TREND',%s,'system',20,1000,1,-1,2.5,0.5,12)", (str(day)+'T00:00:00Z',))
        cur.execute("INSERT INTO trading.equity_curve(portfolio_id,strategy_id,timestamp,portfolio_type,equity) "
            "VALUES('BOOK','LIVE_TREND',%s,'system',1000)", (str(day)+'T00:00:00Z',))
        cur.execute("INSERT INTO trading.run_inputs(portfolio_id,strategy_id,date,config_snapshot) "
            "VALUES('BOOK','LIVE_TREND',%s,'{}')", (day,))
    instrument = dict(symbol="SYN", instrument_type="FUTURE", price_model_number="100",
        adv_model_number="100000", volatility_multiplier_model_number="1", source_id="synthetic-close",
        price_time=str(day-timedelta(days=1))+"T00:00:00Z", history_source_id="synthetic-history",
        history_digest="3"*64, history_observation_count=1, history_complete=True, asset_lookup="exact_symbol",
        baseline_spread_ticks="0", min_spread_ticks="0", max_spread_ticks="0", spread_cost_multiplier="0",
        max_impact_bps="0", tick_size="0.01", point_value="50", max_total_implicit_bps="0")
    market = dict(schema_version="qt-accounting-market/v1", book_id="BOOK", source_day=str(day),
        previous_day=str(day-timedelta(days=1)), valuation_time=str(day)+"T00:00:00Z", currency="USD",
        model_publication_id=MODEL, dataset_source_id="synthetic-bars", dataset_digest="1"*64,
        cost_config_source_id="synthetic-costs", cost_config_digest="2"*64, engine_build="synthetic-input",
        cost_config=dict(explicit_fee_per_contract="2", min_adv="100", min_participation="0", max_participation="0.1"),
        instruments=[instrument])
    source = dict(source_id=MARKET, producer_id="synthetic-execution", policy_version="execution-policy-v1",
        source_version="synthetic-market-v1", as_of=(now-timedelta(seconds=1)).isoformat(),
        valid_until=(now+timedelta(hours=1)).isoformat(), payload=market)
    result = invoke("--market", payload=source)
    assert result.returncode == 0, result.stdout+result.stderr
    result = invoke("--anchor", DECISION, ANCHOR)
    assert result.returncode == 0, result.stdout+result.stderr
    return conn, source


def process():
    return invoke("--first-day", DECISION, ATTEMPT, INPUT, MARKET, ANCHOR)


@pytest.mark.parametrize("desk", ["futures_quiet"], indirect=True)
@pytest.mark.parametrize("connection", ["UTC", "America/New_York"], indirect=True)
def test_first_day_noop_commits_exact_system_opening_and_replays(first_day):
    conn, _ = first_day
    result = process()
    assert result.returncode == 0, result.stdout+result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT daily_pnl,total_pnl,current_portfolio_value,daily_transaction_costs,total_transaction_costs "
                    "FROM trading.live_results WHERE portfolio_type='qt'")
        assert cur.fetchone() == (Decimal(1), Decimal(20), Decimal(1000), Decimal('0.5'), Decimal(12))
        cur.execute("SELECT count(*) FROM trading.executions WHERE portfolio_type='qt'")
        assert cur.fetchone() == (0,)
    after = state(conn)
    result = process()
    assert result.returncode == 0 and "REPLAYED=1" in result.stdout, result.stdout+result.stderr
    assert state(conn) == after


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_first_day_changed_choice_keeps_pnl_and_charges_only_incremental_cost(first_day):
    conn, _ = first_day
    result = process()
    assert result.returncode == 0, result.stdout+result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT daily_pnl,daily_realized_pnl,daily_unrealized_pnl,total_pnl,current_portfolio_value,"
                    "daily_transaction_costs,total_transaction_costs FROM trading.live_results WHERE portfolio_type='qt'")
        assert cur.fetchone() == tuple(map(Decimal, ('-3', '-1', '2.5', '16', '996', '4.5', '16')))
        cur.execute("SELECT strategy_name,side,quantity,total_transaction_costs FROM trading.executions "
                    "WHERE portfolio_type='qt' ORDER BY strategy_name")
        assert cur.fetchall() == [('synthetic-alpha', 'BUY', Decimal(1), Decimal(2)),
                                  ('synthetic-beta', 'SELL', Decimal(1), Decimal(2))]
        cur.execute("SELECT payload->'results'->'currency_totals' FROM trading.qt_execution_observations WHERE observation_id=%s", (INPUT,))
        assert cur.fetchone()[0] == [dict(currency='USD', actual_cash_cost_exact='4',
            daily_unrealized_pnl_exact='2.5', daily_realized_pnl_exact='-1')]


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_first_day_failure_rolls_back_input_positions_financials_and_receipt(first_day):
    conn, _ = first_day
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.reject_first_day_result() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN RAISE EXCEPTION 'synthetic write failure'; END $$;
          CREATE TRIGGER reject_first_day_result BEFORE INSERT ON trading.qt_desk_results
          FOR EACH ROW EXECUTE FUNCTION trading.reject_first_day_result()""")
    before = state(conn)
    result = process()
    assert result.returncode != 0
    assert state(conn) == before
    with conn.cursor() as cur:
        cur.execute("DROP TRIGGER reject_first_day_result ON trading.qt_desk_results")
    result = process()
    assert result.returncode == 0, result.stdout+result.stderr


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
@pytest.mark.parametrize("mutation", ["lease", "instrument", "currency"])
def test_first_day_input_must_match_exact_market_authority(first_day, mutation):
    conn, _ = first_day
    # Re-hash the changed payload, so this exercises authority binding rather
    # than merely detecting an inconsistent content digest.
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.qt_test_canonical(j jsonb) RETURNS text LANGUAGE plpgsql AS $$
          DECLARE result text;
          BEGIN
            IF jsonb_typeof(j)='object' THEN
              SELECT '{'||coalesce(string_agg(to_jsonb(key)::text||':'||trading.qt_test_canonical(value),',' ORDER BY key COLLATE "C"),'')||'}'
                INTO result FROM jsonb_each(j);
            ELSIF jsonb_typeof(j)='array' THEN
              SELECT '['||coalesce(string_agg(trading.qt_test_canonical(value),',' ORDER BY ord),'')||']'
                INTO result FROM jsonb_array_elements(j) WITH ORDINALITY a(value,ord);
            ELSE result := j::text;
            END IF;
            RETURN result;
          END $$""")
        change = {
            'lease': "NEW.valid_until := NEW.valid_until+interval '1 day';",
            'instrument': "NEW.payload := jsonb_set(NEW.payload,'{instruments,0,price_model_number}','\"101\"'::jsonb);",
            'currency': "NEW.payload := jsonb_set(NEW.payload,'{currency}','\"EUR\"'::jsonb);",
        }[mutation]
        cur.execute("CREATE FUNCTION trading.mutate_first_day_input() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
            + change + " NEW.content_digest := encode(sha256(convert_to(trading.qt_test_canonical(NEW.payload),'UTF8')),'hex'); "
            "RETURN NEW; END $$; CREATE TRIGGER mutate_first_day_input BEFORE INSERT ON trading.qt_desk_accounting_inputs "
            "FOR EACH ROW EXECUTE FUNCTION trading.mutate_first_day_input()")
    before = state(conn)
    result = process()
    assert result.returncode != 0, "Contradictory first-day market authority was accepted: "+mutation
    assert state(conn) == before


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
@pytest.mark.parametrize("connection", ["UTC", "America/New_York"], indirect=True)
def test_real_first_day_finalizes_on_day_two_without_invented_predecessor(first_day):
    conn, original_market = first_day
    result = process()
    assert result.returncode == 0, result.stdout+result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT payload FROM trading.desk_run_results WHERE decision_id=%s", (DECISION,))
        original_output = cur.fetchone()[0]
        cur.execute("CREATE OR REPLACE FUNCTION pg_catalog.clock_timestamp() RETURNS timestamptz LANGUAGE sql VOLATILE "
                    "AS $$ SELECT pg_catalog.qt_test_actual_clock_timestamp()+interval '1 day' $$")
        cur.execute("SELECT clock_timestamp()")
        now = cur.fetchone()[0]
        today = now.astimezone(timezone.utc).date().isoformat()
        cur.execute("SELECT to_jsonb(m) FROM trading.qt_model_seed_publications m WHERE publication_id=%s", (MODEL,))
        model = cur.fetchone()[0]
        # Only tomorrow's MODEL is synthetic; yesterday's processed ledger is
        # left exactly as written by the real processor.
        old_day = model['source_day']
        next_model = "a4000000-0000-4000-8000-000000000002"
        model = json.loads(json.dumps(model).replace(old_day, today).replace(MODEL, next_model))
        for seed, proposal, quantity in zip(model['system_components'], model['proposal_components'], ('5','1')):
            key = seed['key']
            for stream, selected in (('system',seed['quantity_exact']),
                                     ('qt_proposal',seed['quantity_exact']), ('qt',quantity)):
                cur.execute("INSERT INTO trading.positions(portfolio_id,strategy_id,strategy_name,date,symbol,"
                    "portfolio_type,quantity,average_price,daily_realized_pnl,daily_unrealized_pnl,last_update) "
                    "VALUES('BOOK','LIVE_TREND',%s,%s,'SYN',%s,%s,100,0,0,%s)",
                    (key['strategy_name'], today, stream, selected, now))
            cur.execute("SELECT qt_proposal_revision::text FROM trading.positions WHERE portfolio_id='BOOK' "
                "AND strategy_name=%s AND date=%s AND portfolio_type='qt_proposal'", (key['strategy_name'],today))
            proposal['position_revision'] = cur.fetchone()[0]
        model['seed_digest'] = qt_digest_v1({'seed_rows': model['system_components']})
        model['proposal_manifest_digest'] = internal_snapshot_digest('qt-proposal-manifest/v1',
            {'proposal_rows': model['proposal_components']})
        cur.execute("INSERT INTO trading.qt_model_seed_publications SELECT * FROM "
            "jsonb_populate_record(NULL::trading.qt_model_seed_publications,%s)", (Json(model),))
    market = deepcopy(original_market)
    next_market = "a2000000-0000-4000-8000-000000000002"
    final = "a5000000-0000-4000-8000-000000000002"
    market.update(source_id=next_market, source_version='synthetic-day-two',
        as_of=(now-timedelta(seconds=1)).isoformat(), valid_until=(now+timedelta(hours=1)).isoformat())
    market['payload'].update(source_day=today, previous_day=old_day,
        valuation_time=today+'T00:00:00Z', model_publication_id=next_model)
    market['payload']['instruments'][0].update(price_model_number='101', price_time=old_day+'T00:00:00Z')
    result = invoke('--market', payload=market)
    assert result.returncode == 0, result.stdout+result.stderr
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.reject_first_day_finalization() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN RAISE EXCEPTION 'synthetic settlement failure'; END $$;
          CREATE TRIGGER reject_first_day_finalization BEFORE INSERT ON trading.qt_desk_finalizations
          FOR EACH ROW EXECUTE FUNCTION trading.reject_first_day_finalization()""")
    before_failure = state(conn)
    result = invoke('--finalize', DECISION, final, next_market)
    assert result.returncode != 0 and 'storage' in result.stdout, result.stdout+result.stderr
    assert state(conn) == before_failure
    with conn.cursor() as cur:
        cur.execute("DROP TRIGGER reject_first_day_finalization ON trading.qt_desk_finalizations")
    before = state(conn)
    result = invoke('--finalize', DECISION, final, next_market)
    assert result.returncode == 0, result.stdout+result.stderr
    transition = json.loads(result.stdout)
    assert transition['first_day_anchor_id'] == ANCHOR
    assert 'predecessor_finalization_source_id' not in transition
    with conn.cursor() as cur:
        cur.execute("SELECT daily_pnl,daily_realized_pnl,daily_unrealized_pnl,total_pnl,current_portfolio_value,"
                    "daily_transaction_costs,total_transaction_costs FROM trading.live_results WHERE portfolio_type='qt'")
        assert cur.fetchone() == tuple(map(Decimal, ('297', '299', '2.5', '316', '1296', '4.5', '16')))
        cur.execute("SELECT payload FROM trading.desk_run_results WHERE decision_id=%s", (DECISION,))
        assert cur.fetchone()[0] == original_output
    after = state(conn)
    assert after != before
    result = invoke('--finalize', DECISION, final, next_market)
    assert result.returncode == 0, result.stdout+result.stderr
    assert state(conn) == after
    confirm_and_process_second_day(conn, old_day, today, next_model, next_market, final)


def confirm_and_process_second_day(conn, old_day, today, next_model, next_market, final):
    """Use the actual API save/evaluate/confirm path, then native continuation."""
    from hashlib import sha256
    from uuid import uuid4
    import psycopg2
    from test_qt_desk_storage import native_evaluator_configuration, canonical_qt_input_bytes
    from algolens.infrastructure.config.dependencies import create_qt_workflow_service
    from algolens.infrastructure.portfolio.qt_workflow_repository import QtWorkflowRepository
    with conn.cursor() as cur:
        cur.execute('CREATE SCHEMA IF NOT EXISTS metadata; CREATE TABLE IF NOT EXISTS metadata.contract_metadata '
            '("Databento Symbol" text, "IB Symbol" text, "Asset Type" text); '
            'TRUNCATE metadata.contract_metadata; INSERT INTO metadata.contract_metadata VALUES (\'SYN\',\'SYN\',\'FUTURE\')')
        cur.execute("SELECT payload FROM trading.qt_evaluation_snapshots WHERE model_publication_id=%s", (MODEL,))
        payload = json.loads(json.dumps(cur.fetchone()[0]).replace(old_day,today).replace(MODEL,next_model))
        cur.execute("SELECT clock_timestamp()")
        now = cur.fetchone()[0]
        cur.execute("INSERT INTO trading.qt_evaluation_snapshots(book_id,source_day,model_publication_id,producer_id,"
            "policy_version,source_version,as_of,valid_until,content_digest,payload) VALUES('BOOK',%s,%s,"
            "'synthetic-desk','desk-policy-v1','synthetic-day-two',%s,%s,%s,%s)",
            (today,next_model,now-timedelta(seconds=1),now+timedelta(hours=1),
             sha256(canonical_qt_input_bytes(payload)).hexdigest(),Json(payload)))
    config = native_evaluator_configuration()
    service = create_qt_workflow_service(QtWorkflowRepository(lambda: psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])),
        evaluator_executable=config['executable'], evaluator_bundle_directory=config['bundle_directory'])
    initial = service.get_draft('BOOK',1).to_wire()
    draft = service.save_draft('BOOK',1,dict(expected_source_digest=initial['source_digest'],
        expected_provenance_digest=initial['provenance_digest'],expected_draft_revision=0,
        idempotency_key=str(uuid4()),rationale='Synthetic second-day quiet continuation.',
        selection_rows=[dict(key=row['key'],quantity_exact='5' if row['key']['strategy_name']=='synthetic-alpha' else '1')
                        for row in initial['selection_rows'] if row['editable']])).to_wire()
    preview = service.create_preview(1,dict(book_id='BOOK',draft_id=draft['draft_id'],draft_revision=draft['draft_revision'],
        draft_digest=draft['draft_digest'],expected_source_digest=draft['source_digest'],
        expected_provenance_digest=draft['provenance_digest'],idempotency_key=str(uuid4()))).to_wire()
    assert preview['confirmable'], (preview['unavailable_reasons'], preview['evaluation'])
    decision = service.confirm_preview(preview['preview_id'],1,dict(action='confirm_selected_book',
        expected_digest=preview['payload_digest'],idempotency_key=str(uuid4()),acknowledge_warnings=True)).to_wire()
    decision_id = decision['decision_id']
    attempt, accounting_input = str(uuid4()),str(uuid4())
    result = invoke('--sourced',decision_id,attempt,accounting_input,next_market,'qt-finalization/'+final)
    assert result.returncode == 0, result.stdout+result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT daily_pnl,total_pnl,current_portfolio_value,daily_transaction_costs "
            "FROM trading.live_results WHERE portfolio_type='qt' AND (date AT TIME ZONE 'UTC')::date=%s", (today,))
        assert cur.fetchone() == tuple(map(Decimal, ('0','316','1296','0')))
        cur.execute("SELECT count(*) FROM trading.executions WHERE portfolio_type='qt' AND date=%s", (today,))
        assert cur.fetchone() == (0,)
    after = state(conn)
    result = invoke('--sourced',decision_id,attempt,accounting_input,next_market,'qt-finalization/'+final)
    assert result.returncode == 0 and 'REPLAYED=1' in result.stdout, result.stdout+result.stderr
    assert state(conn) == after
